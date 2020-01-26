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
#include "apps/sntp/sntp.h"

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

#include <sys/time.h>

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* Constants that aren't configurable in menuconfig */
#define HOST "192.168.1.148"
#define PORT 1704
#define BUFF_LEN 6000
#define ENVELOPE_FILTER_BUFFER_SIZE (1764)

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
static char mac_address[18];

static esp_err_t envelope_filter_destroy(audio_element_handle_t self) {
    return ESP_OK;
}

static esp_err_t envelope_filter_open(audio_element_handle_t self) {
    return ESP_OK;
}

static esp_err_t envelope_filter_close(audio_element_handle_t self) {
  if (audio_element_get_state(self) != AEL_STATE_PAUSED) {
    audio_element_info_t info = {0};
    audio_element_getinfo(self, &info);
    info.byte_pos = 0;
    audio_element_setinfo(self, &info);
  }

  return ESP_OK;
}

static double current_volume = 0.5;

static void apply_envelope(void *buffer, int size) {
  // assume 16 bit samples:
  int16_t *samples = buffer;
  size_t count = size / 2;

  for (size_t i = 0; i < count; i++) {
    samples[i] = samples[i] * current_volume;
  }
}

static int envelope_filter_process(audio_element_handle_t self, char *buffer, int size) {
  audio_element_info_t info = { 0 };
  audio_element_getinfo(self, &info);

  int size_read = audio_element_input(self, buffer, size);
  int result = size_read; // Could be negative error code

  if (size_read > 0) {
    apply_envelope(buffer, size_read);
    int size_write = audio_element_output(self, buffer, size_read);
    if (size_write > 0) {
      info.byte_pos += size_write;
    }
    result = size_write;
  }

  audio_element_setinfo(self, &info);
  return result;
}

audio_element_handle_t create_envelope_filter() {
  audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();

  cfg.process = envelope_filter_process;
  cfg.open = envelope_filter_open;
  cfg.close = envelope_filter_close;
  cfg.destroy = envelope_filter_destroy;

  cfg.buffer_len = ENVELOPE_FILTER_BUFFER_SIZE;

  cfg.tag = "volume";
  cfg.task_prio = 5;
  cfg.task_core = 0;
  cfg.out_rb_size = 3 * ENVELOPE_FILTER_BUFFER_SIZE;
  audio_element_handle_t el = audio_element_init(&cfg);
  mem_assert(el);
  audio_element_info_t info = {0};
  audio_element_setinfo(el, &info);

  return el;
}

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
    int result, size, id_counter;
    struct timeval now, tv1, tv2, tv3, last_time_sync;
    time_message_t time_message;
    double time_diff;

    last_time_sync.tv_sec = 0;
    last_time_sync.tv_usec = 0;
    id_counter = 0;

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
        server_settings_message_t server_settings_message;

        result = gettimeofday(&now, NULL);
        if (result) {
            ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
            return;
        }

        bool received_header = false;
        base_message_t base_message = {
            SNAPCAST_MESSAGE_HELLO,
            0x0,
            0x0,
            { now.tv_sec, now.tv_usec },
            { 0x0, 0x0 },
            0x0,
        };

        hello_message_t hello_message = {
            mac_address,
            "ESP32-Caster",
            "0.0.0",
            "libsnapcast",
            "esp32",
            "xtensa",
            1,
            mac_address,
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
            size = 0;
            while (size < BASE_MESSAGE_SIZE) {
                result = read(sockfd, &(buff[size]), BASE_MESSAGE_SIZE - size);
                if (result < 0) {
                    ESP_LOGI(TAG, "Failed to read from server: %d\r\n", result);
                    return;
                }

                size += result;
            }

            result = gettimeofday(&now, NULL);
            if (result) {
                ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
                return;
            }

            result = base_message_deserialize(&base_message, buff, size);
            if (result) {
                ESP_LOGI(TAG, "Failed to read base message: %d\r\n", result);
                // TODO there should be a big circular buffer or something for this
                return;
            }

            base_message.received.sec = now.tv_sec;
            base_message.received.usec = now.tv_usec;

            start = buff;
            size = 0;
            while (size < base_message.size) {
                result = read(sockfd, &(buff[size]), base_message.size - size);
                if (result < 0) {
                    ESP_LOGI(TAG, "Failed to read from server: %d\r\n", result);
                    return;
                }

                size += result;
            }
            ///print_buffer(start, size);
            ///ESP_LOGI(TAG, "\r\n");

            switch (base_message.type) {
                case SNAPCAST_MESSAGE_CODEC_HEADER:
                    result = codec_header_message_deserialize(&codec_header_message, start, size);
                    if (result) {
                        ESP_LOGI(TAG, "Failed to read codec header: %d\r\n", result);
                        return;
                    }

                    //ESP_LOGI(TAG, "Received codec header message\r\n");
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

                case SNAPCAST_MESSAGE_WIRE_CHUNK:
                    if (!received_header) {
                        continue;
                    }

                    result = wire_chunk_message_deserialize(&wire_chunk_message, start, size);
                    if (result) {
                        ESP_LOGI(TAG, "Failed to read wire chunk: %d\r\n", result);
                        return;
                    }

                    //ESP_LOGI(TAG, "Received wire message\r\n");
                    size = wire_chunk_message.size;
                    start = wire_chunk_message.payload;

                    while (size > 0) {
                        result = raw_stream_write(snapcast_stream, start, size);
                        start += result; // TODO pointer arithmetic is bad maybe?
                        size -= result;
                    }

                    wire_chunk_message_free(&wire_chunk_message);
                break;

                case SNAPCAST_MESSAGE_SERVER_SETTINGS:
                    // The first 4 bytes in the buffer are the size of the string.
                    // We don't need this, so we'll shift the entire buffer over 4 bytes
                    // and use the extra room to add a null character so cJSON can pares it.
                    memmove(start, start + 4, size - 4);
                    start[size - 3] = '\0';
                    result = server_settings_message_deserialize(&server_settings_message, start);
                    if (result) {
                        ESP_LOGI(TAG, "Failed to read server settings: %d\r\n", result);
                        return;
                    }

                    ESP_LOGI(TAG, "Setting volume: %d", server_settings_message.volume);
                    current_volume = server_settings_message.volume / 100.0;
                break;

                case SNAPCAST_MESSAGE_TIME:
                    result = time_message_deserialize(&time_message, start, size);
                    if (result) {
                        ESP_LOGI(TAG, "Failed to deserialize time message\r\n");
                        return;
                    }

                    // tv == server to client latency (s2c)
                    // time_message.latency == client to server latency(c2s)
                    // TODO the fact that I have to do this simple conversion means
                    // I should probably use the timeval struct instead of my own
                    tv1.tv_sec = base_message.received.sec;
                    tv1.tv_usec = base_message.received.usec;
                    tv3.tv_sec = base_message.sent.sec;
                    tv3.tv_usec = base_message.sent.usec;
                    timersub(&tv1, &tv3, &tv2);
                    tv1.tv_sec = time_message.latency.sec;
                    tv1.tv_usec = time_message.latency.usec;

                    // tv1 == c2s: client to server
                    // tv2 == s2c: server to client
                    //ESP_LOGI(TAG, "c2s: %ld %ld\r\n", tv1.tv_sec, tv1.tv_usec);
                    //ESP_LOGI(TAG, "s2c: %ld %ld\r\n", tv2.tv_sec, tv2.tv_usec);
                    time_diff = (((double)(tv1.tv_sec - tv2.tv_sec) / 2) * 1000) + (((double)(tv1.tv_usec - tv2.tv_usec) / 2) / 1000);
                    ESP_LOGI(TAG, "Current latency: %fms\r\n", time_diff);
                break;
            }

            // If it's been a second or longer since our last time message was
            // sent, do so now
            result = gettimeofday(&now, NULL);
            if (result) {
                ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
                return;
            }
            timersub(&now, &last_time_sync, &tv1);
            if (tv1.tv_sec >= 1) {
                last_time_sync.tv_sec = now.tv_sec;
                last_time_sync.tv_usec = now.tv_usec;

                base_message.type = SNAPCAST_MESSAGE_TIME;
                base_message.id = id_counter++;
                base_message.refersTo = 0;
                base_message.received.sec = 0;
                base_message.received.usec = 0;
                base_message.sent.sec = now.tv_sec;
                base_message.sent.usec = now.tv_usec;
                base_message.size = TIME_MESSAGE_SIZE;

                result = base_message_serialize(
                    &base_message,
                    base_message_serialized,
                    BASE_MESSAGE_SIZE
                );
                if (result) {
                    ESP_LOGE(TAG, "Failed to serialize base message for time\r\n");
                    continue;
                }

                result = time_message_serialize(&time_message, buff, BUFF_LEN);
                if (result) {
                    ESP_LOGI(TAG, "Failed to serialize time message\r\b");
                    continue;
                }

                write(sockfd, base_message_serialized, BASE_MESSAGE_SIZE);
                write(sockfd, buff, TIME_MESSAGE_SIZE);
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

void set_time_from_sntp() {
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    char strftime_buf[64];

    // Set timezone to Eastern Standard Time and print local time
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in UTC is: %s", strftime_buf);
}

void app_main(void)
{
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, flac_decoder, volume;
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

    ESP_LOGI(TAG, "[2.2] Create flac decoder to decode flac file");
    flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
    flac_decoder = flac_decoder_init(&flac_cfg);

    ESP_LOGI(TAG, "[2.3] Create volume filter");
    volume = create_envelope_filter();

    ESP_LOGI(TAG, "[2.4] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.i2s_config.sample_rate = 44100;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.5] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, snapcast_stream, "snapcast");
    audio_pipeline_register(pipeline, flac_decoder, "flac");
    audio_pipeline_register(pipeline, volume, "volume");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.6] Link it together snapcast-->flac_decoder-->volume-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"snapcast", "flac", "volume", "i2s"}, 4);

    ESP_LOGI(TAG, "[ 3 ] Setup event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[3.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    initialise_wifi();
    set_time_from_sntp();
    xTaskCreatePinnedToCore(&http_get_task, "http_get_task", 4096, NULL, 5, NULL, 1);

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
