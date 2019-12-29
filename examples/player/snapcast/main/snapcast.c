/* Play flac file by audio pipeline

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "flac_decoder.h"
#include "audio_hal.h"
#include "raw_stream.h"

#include "snapcast.h"

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* Constants that aren't configurable in menuconfig */
#define HOST "192.168.1.141"
#define PORT 1704
#define BUFF_LEN 6000

/* Logging tag */
static const char *TAG = "SNAPCAST";

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static char buff[BUFF_LEN];
static audio_element_handle_t snapcast_stream;

int flac_music_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
    return AEL_IO_DONE;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void http_get_task(void *pvParameters)
{
    struct sockaddr_in servaddr;
	char *start;
	int sockfd;
	char base_message_serialized[BASE_MESSAGE_SIZE];
	char *hello_message_serialized;
	int result, size;

    while(1) {
        /* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                            false, true, portMAX_DELAY);
        ESP_LOGI(TAG, "Connected to AP");

		servaddr.sin_family = AF_INET;
		servaddr.sin_addr.s_addr = inet_addr(HOST);
		servaddr.sin_port = htons(PORT);

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

		if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(sockfd);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");

		codec_header_message_t codec_header_message;
		wire_chunk_message_t wire_chunk_message;
		
		bool received_header = false;
		base_message_t base_message = {
			hello,
			0x0,
			0x0,
			{ 0x5df65146, 0x0 },
			{ 0x5df65146, 0x0 },
			0x0,
		};

		hello_message_t hello_message = {
			"0c:8b:fd:d0:e4:d1",
			"ESP32-Caster",
			"0.0.0",
			"libsnapcast",
			"esp32",
			"xtensa",
			1,
			"0c:8b:fd:d0:e4:d1",
			2,
		};

		hello_message_serialized = hello_message_serialize(&hello_message, (size_t*) &(base_message.size));
		if (!hello_message_serialized) {
			ESP_LOGI(TAG, "Failed to serialize hello message\r\b");
			return;
		}

		result = base_message_serialize(
			&base_message,
			base_message_serialized,
			BASE_MESSAGE_SIZE
		);
		if (result) {
			ESP_LOGI(TAG, "Failed to serialize base message\r\n");
			return;
		}

		write(sockfd, base_message_serialized, BASE_MESSAGE_SIZE);
		write(sockfd, hello_message_serialized, base_message.size);
		free(hello_message_serialized);

		for (;;) {
			size = read(sockfd, buff, BUFF_LEN);
			if (size < 0) {
				ESP_LOGI(TAG, "Failed to read from server: %d\r\n", size);
				return;
			}

			result = base_message_deserialize(&base_message, buff, size);
			if (result) {
				ESP_LOGI(TAG, "Failed to read base message: %d\r\n", result);
				// TODO there should be a big circular buffer or something for this
				return;
			}

			start = &(buff[BASE_MESSAGE_SIZE]);
			size -= BASE_MESSAGE_SIZE;
			///print_buffer(start, size);
			///ESP_LOGI(TAG, "\r\n");

			switch (base_message.type) {
				case codec_header:
					result = codec_header_message_deserialize(&codec_header_message, start, size);
					if (result) {
						ESP_LOGI(TAG, "Failed to read codec header: %d\r\n", result);
						return;
					}

					ESP_LOGI(TAG, "Received codec header message\r\n");
                    size = codec_header_message.size;
                    start = codec_header_message.payload;

                    while (size > 0) {
                        result = raw_stream_write(snapcast_stream, start, size);
                        start += result; // TODO pointer arithmetic is bad maybe?
                        size -= result;
                    }

					codec_header_message_free(&codec_header_message);
					received_header = true;
				break;
				
				case wire_chunk:
					if (!received_header) {
						continue;
					}

					result = wire_chunk_message_deserialize(&wire_chunk_message, start, size);
					if (result) {
						ESP_LOGI(TAG, "Failed to read wire chunk: %d\r\n", result);
						return;
					}

					ESP_LOGI(TAG, "Received wire message\r\n");
                    size = wire_chunk_message.size;
                    start = wire_chunk_message.payload;

                    while (size > 0) {
                        result = raw_stream_write(snapcast_stream, start, size);
                        start += result; // TODO pointer arithmetic is bad maybe?
                        size -= result;
                    }
					
					wire_chunk_message_free(&wire_chunk_message);
				break;
			}
		}
        
		ESP_LOGI(TAG, "... done reading from socket\r\n");
        close(sockfd);
        for(int countdown = 10; countdown >= 0; countdown--) {
            ESP_LOGI(TAG, "%d... ", countdown);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Starting again!");
    }
}

void app_main(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, flac_decoder;
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    
    audio_hal_codec_config_t audio_hal_codec_cfg = AUDIO_HAL_ES8388_DEFAULT();
    audio_hal_handle_t hal = audio_hal_init(&audio_hal_codec_cfg, 0);
    audio_hal_ctrl_codec(hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline, add all elements to pipeline, and subscribe pipeline event");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create snapcast stream");
    raw_stream_cfg_t snapcast_stream_cfg;
    snapcast_stream_cfg.type = AUDIO_STREAM_WRITER; 
    snapcast_stream_cfg.out_rb_size = 8 * 1024;
    snapcast_stream = raw_stream_init(&snapcast_stream_cfg);

    ESP_LOGI(TAG, "[2.2] Create flac decoder to decode flac file and set custom read callback");
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    flac_decoder = flac_decoder_init(&flac_cfg);

    ESP_LOGI(TAG, "[2.3] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.i2s_config.sample_rate = 48000;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, snapcast_stream, "snapcast");
    audio_pipeline_register(pipeline, flac_decoder, "flac");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together snapcast-->flac_decoder-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"snapcast", "flac", "i2s"}, 3);

    ESP_LOGI(TAG, "[ 3 ] Setup event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[3.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    initialise_wifi();
    xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) flac_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(flac_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from flac decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates , music_info.bits, music_info.channels);
            continue;
        }
        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int) msg.data == AEL_STATUS_STATE_STOPPED) {
            break;
        }
    }

    ESP_LOGI(TAG, "[ 5 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, flac_decoder);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Make sure audio_pipeline_remove_listener is called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, flac_decoder);
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(flac_decoder);
}
