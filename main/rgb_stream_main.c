#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box2.h"
#include "box2_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hardware_test_screen.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STREAM_WIDTH BOX2_LCD_WIDTH
#define STREAM_HEIGHT BOX2_LCD_HEIGHT
#define STREAM_PORT 5000
#define DISCOVERY_PORT 5001
#define STREAM_BUFFER_COUNT 3
#define STREAM_FRAME_BYTES (STREAM_WIDTH * STREAM_HEIGHT * sizeof(uint16_t))
#define STREAM_MAX_JPEG_BYTES STREAM_FRAME_BYTES
#define STREAM_FRAGMENT_PAYLOAD_BYTES 1400
#define STREAM_MAX_FRAGMENTS \
    ((STREAM_MAX_JPEG_BYTES + STREAM_FRAGMENT_PAYLOAD_BYTES - 1) / \
     STREAM_FRAGMENT_PAYLOAD_BYTES)
#define STREAM_FRAGMENT_BITMAP_BYTES ((STREAM_MAX_FRAGMENTS + 7) / 8)
#define LCD_FRAME_BYTES (BOX2_LCD_WIDTH * BOX2_LCD_HEIGHT * sizeof(uint16_t))

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "mjpeg_stream";
static const char *WIFI_NAMESPACE = "rgb_wifi";

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_free_buffers;
static QueueHandle_t s_ready_frames;
static uint8_t *s_frame_buffers[STREAM_BUFFER_COUNT];
static uint8_t *s_jpeg_buffer;
static jpeg_dec_handle_t s_jpeg_decoder;

static volatile uint32_t s_received_frames;
static volatile uint32_t s_displayed_frames;
static volatile uint32_t s_dropped_frames;
static volatile uint64_t s_received_bytes;
static volatile uint64_t s_display_time_us;
static volatile uint64_t s_decode_time_us;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t reserved;
} discovery_request_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t stream_port;
    uint32_t reserved;
} discovery_reply_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t pixel_format;
} stream_hello_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t status;
} stream_ack_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t frame_bytes;
    uint32_t sequence;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint32_t fragment_offset;
    uint16_t payload_bytes;
    uint16_t reserved;
} udp_fragment_header_t;

static void trim_line(char *line)
{
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t load_credentials(wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_length = sizeof(credentials->ssid);
    size_t password_length = sizeof(credentials->password);
    err = nvs_get_str(handle, "ssid", credentials->ssid, &ssid_length);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", credentials->password,
                          &password_length);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_credentials(const wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open credential NVS");
    esp_err_t err = nvs_set_str(handle, "ssid", credentials->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", credentials->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t provision_credentials(wifi_credentials_t *credentials)
{
    char line[128];
    printf("BOX2_PROVISION_READY\n");
    printf("Send WIFI_SSID:<ssid> and WIFI_PASS:<password> over USB serial.\n");
    fflush(stdout);

    bool have_ssid = false;
    bool have_password = false;
    while (!have_ssid || !have_password) {
        if (!fgets(line, sizeof(line), stdin)) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        trim_line(line);
        if (strncmp(line, "WIFI_SSID:", 10) == 0) {
            strlcpy(credentials->ssid, line + 10, sizeof(credentials->ssid));
            have_ssid = credentials->ssid[0] != '\0';
            printf("BOX2_SSID_ACCEPTED\n");
            fflush(stdout);
        } else if (strncmp(line, "WIFI_PASS:", 10) == 0) {
            strlcpy(credentials->password, line + 10,
                    sizeof(credentials->password));
            have_password = strlen(credentials->password) >= 8;
            printf(have_password ? "BOX2_PASS_ACCEPTED\n" :
                                   "BOX2_PASS_REJECTED\n");
            fflush(stdout);
        }
    }
    return save_credentials(credentials);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        printf("BOX2_IP=" IPSTR "\n", IP2STR(&event->ip_info.ip));
        fflush(stdout);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(const wifi_credentials_t *credentials)
{
    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG,
                        "create Wi-Fi event group");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize TCP/IP");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "create event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_FAIL, TAG,
                        "create Wi-Fi station netif");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "register IP event handler");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, credentials->ssid,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, credentials->password,
            sizeof(config.sta.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.failure_retry_cnt = 3;

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "set Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "set Wi-Fi configuration");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "disable Wi-Fi power saving");

    ESP_LOGI(TAG, "Connecting to SSID '%s'", credentials->ssid);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    return ESP_OK;
}

static void fill_pattern(uint16_t *pixels, int width, int height)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint16_t red = (uint16_t)((x * 31 / width) << 11);
            uint16_t green = (uint16_t)((y * 63 / height) << 5);
            uint16_t blue = (uint16_t)(((x + y) * 31 / (width + height)));
            pixels[y * width + x] = red | green | blue;
        }
    }
}

static esp_err_t run_lcd_benchmark(const char *name, int x, int y, int width,
                                   int height, uint16_t *pixels, int frames)
{
    ESP_RETURN_ON_ERROR(box2_lcd_draw_bitmap(x, y, x + width, y + height,
                                             pixels),
                        TAG, "warm up LCD benchmark");
    int64_t start = esp_timer_get_time();
    for (int frame = 0; frame < frames; ++frame) {
        ESP_RETURN_ON_ERROR(box2_lcd_draw_bitmap(x, y, x + width, y + height,
                                                 pixels),
                            TAG, "draw LCD benchmark frame");
    }
    int64_t elapsed = esp_timer_get_time() - start;
    double fps = (double)frames * 1000000.0 / (double)elapsed;
    double throughput = ((double)width * height * sizeof(uint16_t) * frames * 8.0) /
                        (double)elapsed;
    ESP_LOGI(TAG,
             "LCD_BENCH name=%s size=%dx%d frames=%d elapsed_ms=%.1f fps=%.2f "
             "payload_mbps=%.2f",
             name, width, height, frames, elapsed / 1000.0, fps, throughput);
    return ESP_OK;
}

static esp_err_t initialize_frame_buffers(void)
{
    s_free_buffers = xQueueCreate(STREAM_BUFFER_COUNT, sizeof(uint8_t));
    s_ready_frames = xQueueCreate(1, sizeof(uint8_t));
    ESP_RETURN_ON_FALSE(s_free_buffers && s_ready_frames, ESP_ERR_NO_MEM, TAG,
                        "create frame queues");

    for (uint8_t index = 0; index < STREAM_BUFFER_COUNT; ++index) {
        s_frame_buffers[index] = heap_caps_aligned_alloc(
            16, STREAM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_frame_buffers[index], ESP_ERR_NO_MEM, TAG,
                            "allocate RGB565 frame buffer");
        fill_pattern((uint16_t *)s_frame_buffers[index], STREAM_WIDTH,
                     STREAM_HEIGHT);
        xQueueSend(s_free_buffers, &index, 0);
    }
    s_jpeg_buffer = heap_caps_malloc(STREAM_MAX_JPEG_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_jpeg_buffer, ESP_ERR_NO_MEM, TAG,
                        "allocate MJPEG input buffer");

    jpeg_dec_config_t decoder_config = DEFAULT_JPEG_DEC_CONFIG();
    decoder_config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    ESP_RETURN_ON_FALSE(jpeg_dec_open(&decoder_config, &s_jpeg_decoder) ==
                            JPEG_ERR_OK,
                        ESP_FAIL, TAG, "open JPEG decoder");
    return ESP_OK;
}

static uint8_t acquire_receive_buffer(void)
{
    uint8_t index;
    xQueueReceive(s_free_buffers, &index, portMAX_DELAY);
    return index;
}

static void queue_completed_frame(uint8_t index)
{
    uint8_t stale_index;
    if (xQueueReceive(s_ready_frames, &stale_index, 0) == pdTRUE) {
        ++s_dropped_frames;
        xQueueSend(s_free_buffers, &stale_index, portMAX_DELAY);
    }
    xQueueSend(s_ready_frames, &index, portMAX_DELAY);
}

static bool decode_jpeg_frame(size_t jpeg_size, uint8_t *destination)
{
    jpeg_dec_io_t io = {
        .inbuf = s_jpeg_buffer,
        .inbuf_len = (int)jpeg_size,
        .outbuf = destination,
    };
    jpeg_dec_header_info_t info = {0};
    if (jpeg_dec_parse_header(s_jpeg_decoder, &io, &info) != JPEG_ERR_OK ||
        info.width != STREAM_WIDTH || info.height != STREAM_HEIGHT) {
        return false;
    }
    int output_size = 0;
    if (jpeg_dec_get_outbuf_len(s_jpeg_decoder, &output_size) != JPEG_ERR_OK ||
        output_size != STREAM_FRAME_BYTES) {
        return false;
    }
    return jpeg_dec_process(s_jpeg_decoder, &io) == JPEG_ERR_OK;
}

static void display_task(void *argument)
{
    (void)argument;
    while (true) {
        uint8_t index;
        if (xQueueReceive(s_ready_frames, &index, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int64_t start = esp_timer_get_time();
        esp_err_t err = box2_lcd_draw_bitmap(0, 0, STREAM_WIDTH, STREAM_HEIGHT,
                                             (uint16_t *)s_frame_buffers[index]);
        s_display_time_us += (uint64_t)(esp_timer_get_time() - start);
        if (err == ESP_OK) {
            ++s_displayed_frames;
        } else {
            ESP_LOGE(TAG, "LCD frame failed: %s", esp_err_to_name(err));
        }
        xQueueSend(s_free_buffers, &index, portMAX_DELAY);
    }
}

static void statistics_task(void *argument)
{
    (void)argument;
    uint32_t last_received = 0;
    uint32_t last_displayed = 0;
    uint32_t last_dropped = 0;
    uint64_t last_bytes = 0;
    uint64_t last_display_time = 0;
    uint64_t last_decode_time = 0;
    int64_t last_time = esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t now = esp_timer_get_time();
        double seconds = (now - last_time) / 1000000.0;
        uint32_t received = s_received_frames;
        uint32_t displayed = s_displayed_frames;
        uint32_t dropped = s_dropped_frames;
        uint64_t bytes = s_received_bytes;
        uint64_t display_time = s_display_time_us;
        uint64_t decode_time = s_decode_time_us;
        uint32_t receive_delta = received - last_received;
        uint32_t display_delta = displayed - last_displayed;
        double average_lcd_ms = display_delta ?
            (display_time - last_display_time) / 1000.0 / display_delta : 0.0;
        double average_decode_ms = receive_delta ?
            (decode_time - last_decode_time) / 1000.0 / receive_delta : 0.0;

        ESP_LOGI(TAG,
                 "STREAM_STATS rx_fps=%.2f lcd_fps=%.2f drop_fps=%.2f "
                 "rx_mbps=%.2f jpeg_ms=%.2f lcd_ms=%.2f totals=%" PRIu32
                 "/%" PRIu32 "/%" PRIu32,
                 receive_delta / seconds,
                 display_delta / seconds,
                 (dropped - last_dropped) / seconds,
                 ((bytes - last_bytes) * 8.0 / 1000000.0) / seconds,
                 average_decode_ms, average_lcd_ms,
                 received, displayed, dropped);

        last_received = received;
        last_displayed = displayed;
        last_dropped = dropped;
        last_bytes = bytes;
        last_display_time = display_time;
        last_decode_time = decode_time;
        last_time = now;
    }
}

static void stream_server_task(void *argument)
{
    (void)argument;
    while (true) {
        int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd < 0) {
            ESP_LOGE(TAG, "UDP stream socket failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int reuse = 1;
        int receive_buffer_size = STREAM_FRAME_BYTES * 2;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
                   sizeof(receive_buffer_size));
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(STREAM_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
            ESP_LOGE(TAG, "UDP stream bind failed: errno=%d", errno);
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "MJPEG UDP server ready on port %d", STREAM_PORT);
        printf("BOX2_UDP_READY=%d\n", STREAM_PORT);
        fflush(stdout);

        uint8_t datagram[sizeof(udp_fragment_header_t) +
                         STREAM_FRAGMENT_PAYLOAD_BYTES];
        uint8_t fragment_bitmap[STREAM_FRAGMENT_BITMAP_BYTES] = {0};
        struct sockaddr_in active_peer = {0};
        bool have_peer = false;
        bool have_sequence = false;
        bool frame_in_progress = false;
        uint32_t current_sequence = 0;
        uint32_t current_frame_bytes = 0;
        uint16_t current_fragment_count = 0;
        uint16_t received_fragments = 0;

        while (true) {
            struct sockaddr_in peer = {0};
            socklen_t peer_length = sizeof(peer);
            int received = recvfrom(socket_fd, datagram, sizeof(datagram), 0,
                                    (struct sockaddr *)&peer, &peer_length);
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received < 0) {
                ESP_LOGW(TAG, "UDP stream receive failed: errno=%d", errno);
                break;
            }

            if (received == sizeof(stream_hello_t)) {
                const stream_hello_t *hello = (const stream_hello_t *)datagram;
                if (memcmp(hello->magic, "B2DS", 4) == 0 &&
                    ntohs(hello->version) == 4 &&
                    ntohs(hello->width) == STREAM_WIDTH &&
                    ntohs(hello->height) == STREAM_HEIGHT &&
                    ntohs(hello->pixel_format) == 2) {
                    bool new_peer = !have_peer ||
                        peer.sin_addr.s_addr != active_peer.sin_addr.s_addr ||
                        peer.sin_port != active_peer.sin_port;
                    active_peer = peer;
                    have_peer = true;
                    if (new_peer) {
                        have_sequence = false;
                        frame_in_progress = false;
                    }
                    stream_ack_t ack = {
                        .magic = {'B', '2', 'D', 'A'},
                        .version = htons(4),
                        .status = htons(0),
                    };
                    sendto(socket_fd, &ack, sizeof(ack), 0,
                           (struct sockaddr *)&peer, peer_length);
                    if (new_peer) {
                        ESP_LOGI(TAG, "UDP stream client accepted from %s:%u",
                                 inet_ntoa(peer.sin_addr),
                                 ntohs(peer.sin_port));
                    }
                }
                continue;
            }

            if (!have_peer || peer.sin_addr.s_addr != active_peer.sin_addr.s_addr ||
                peer.sin_port != active_peer.sin_port ||
                received < (int)sizeof(udp_fragment_header_t)) {
                continue;
            }

            const udp_fragment_header_t *header =
                (const udp_fragment_header_t *)datagram;
            uint32_t frame_bytes = ntohl(header->frame_bytes);
            uint32_t sequence = ntohl(header->sequence);
            uint16_t fragment_index = ntohs(header->fragment_index);
            uint16_t fragment_count = ntohs(header->fragment_count);
            uint32_t fragment_offset = ntohl(header->fragment_offset);
            uint16_t payload_bytes = ntohs(header->payload_bytes);
            uint16_t expected_count = (uint16_t)(
                (frame_bytes + STREAM_FRAGMENT_PAYLOAD_BYTES - 1) /
                STREAM_FRAGMENT_PAYLOAD_BYTES);
            uint32_t remaining_bytes = fragment_offset < frame_bytes
                ? frame_bytes - fragment_offset : 0;
            uint16_t expected_payload = (uint16_t)(
                remaining_bytes > STREAM_FRAGMENT_PAYLOAD_BYTES
                    ? STREAM_FRAGMENT_PAYLOAD_BYTES : remaining_bytes);
            bool valid_fragment = memcmp(header->magic, "B2U4", 4) == 0 &&
                frame_bytes > 0 && frame_bytes <= STREAM_MAX_JPEG_BYTES &&
                fragment_count > 0 && fragment_count <= STREAM_MAX_FRAGMENTS &&
                fragment_count == expected_count &&
                fragment_index < fragment_count &&
                fragment_offset ==
                    (uint32_t)fragment_index * STREAM_FRAGMENT_PAYLOAD_BYTES &&
                fragment_offset < frame_bytes &&
                payload_bytes == expected_payload &&
                received == (int)(sizeof(*header) + payload_bytes);
            if (!valid_fragment) {
                continue;
            }

            if (!have_sequence || (int32_t)(sequence - current_sequence) > 0) {
                if (frame_in_progress) {
                    ++s_dropped_frames;
                }
                current_sequence = sequence;
                current_frame_bytes = frame_bytes;
                current_fragment_count = fragment_count;
                received_fragments = 0;
                memset(fragment_bitmap, 0, sizeof(fragment_bitmap));
                have_sequence = true;
                frame_in_progress = true;
            } else if ((int32_t)(sequence - current_sequence) < 0 ||
                       !frame_in_progress) {
                continue;
            }

            if (frame_bytes != current_frame_bytes ||
                fragment_count != current_fragment_count) {
                continue;
            }
            uint8_t mask = (uint8_t)(1U << (fragment_index & 7));
            uint8_t *bitmap_byte = &fragment_bitmap[fragment_index >> 3];
            if ((*bitmap_byte & mask) != 0) {
                continue;
            }
            memcpy(s_jpeg_buffer + fragment_offset,
                   datagram + sizeof(*header), payload_bytes);
            *bitmap_byte |= mask;
            ++received_fragments;
            s_received_bytes += payload_bytes;

            if (received_fragments != current_fragment_count) {
                continue;
            }
            frame_in_progress = false;
            uint8_t index = acquire_receive_buffer();
            int64_t decode_start = esp_timer_get_time();
            bool decoded = decode_jpeg_frame(current_frame_bytes,
                                             s_frame_buffers[index]);
            s_decode_time_us +=
                (uint64_t)(esp_timer_get_time() - decode_start);
            ++s_received_frames;
            if (!decoded) {
                ESP_LOGW(TAG, "Rejected invalid reassembled MJPEG frame");
                ++s_dropped_frames;
                xQueueSend(s_free_buffers, &index, portMAX_DELAY);
                continue;
            }
            queue_completed_frame(index);
        }

        close(socket_fd);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void discovery_task(void *argument)
{
    (void)argument;
    while (true) {
        int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd < 0) {
            ESP_LOGE(TAG, "discovery socket failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int reuse = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(DISCOVERY_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
            ESP_LOGE(TAG, "discovery bind failed: errno=%d", errno);
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Discovery service ready on UDP port %d", DISCOVERY_PORT);
        while (true) {
            discovery_request_t request;
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);
            int received = recvfrom(socket_fd, &request, sizeof(request), 0,
                                    (struct sockaddr *)&peer, &peer_length);
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received != sizeof(request) ||
                memcmp(request.magic, "B2DQ", 4) != 0 ||
                ntohs(request.version) != 1) {
                continue;
            }

            discovery_reply_t reply = {
                .magic = {'B', '2', 'D', 'R'},
                .version = htons(1),
                .width = htons(STREAM_WIDTH),
                .height = htons(STREAM_HEIGHT),
                .stream_port = htons(STREAM_PORT),
                .reserved = 0,
            };
            sendto(socket_fd, &reply, sizeof(reply), 0,
                   (struct sockaddr *)&peer, peer_length);
        }
    }
}

static void show_waiting_screen(void)
{
    const char *lines[] = {
        "MODE WINDOWS EXT DISPLAY",
        "FRAME 320X240 MJPEG",
        "UDP VIDEO PORT 5000",
        "AUTO DISCOVERY UDP 5001",
        "JPEG QUALITY 80",
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hardware_test_screen_show_lines("BOX2 STREAM BENCH", lines,
                                        sizeof(lines) / sizeof(lines[0]), 0));
}

void app_main(void)
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(box2_board_init());
    ESP_ERROR_CHECK(box2_lcd_init());
    ESP_ERROR_CHECK(box2_lcd_set_backlight(100));

    uint16_t *full_frame = heap_caps_malloc(LCD_FRAME_BYTES,
                                             MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(full_frame ? ESP_OK : ESP_ERR_NO_MEM);
    fill_pattern(full_frame, BOX2_LCD_WIDTH, BOX2_LCD_HEIGHT);
    ESP_ERROR_CHECK(run_lcd_benchmark("full", 0, 0, BOX2_LCD_WIDTH,
                                      BOX2_LCD_HEIGHT, full_frame, 30));
    free(full_frame);

    ESP_ERROR_CHECK(initialize_frame_buffers());
    ESP_ERROR_CHECK(run_lcd_benchmark("stream-region", 0, 0,
                                      STREAM_WIDTH, STREAM_HEIGHT,
                                      (uint16_t *)s_frame_buffers[0], 60));
    show_waiting_screen();

    wifi_credentials_t credentials = {0};
    if (load_credentials(&credentials) != ESP_OK) {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials; waiting for USB provisioning");
        ESP_ERROR_CHECK(provision_credentials(&credentials));
    }
    ESP_LOGI(TAG, "Using Wi-Fi SSID '%s' (password length %u)", credentials.ssid,
             (unsigned)strlen(credentials.password));
    ESP_ERROR_CHECK(connect_wifi(&credentials));

    BaseType_t display_created = xTaskCreatePinnedToCore(
        display_task, "mjpeg_display", 4096, NULL, 12, NULL, 1);
    BaseType_t stats_created = xTaskCreate(
        statistics_task, "rgb_stats", 4096, NULL, 5, NULL);
    BaseType_t server_created = xTaskCreatePinnedToCore(
        stream_server_task, "mjpeg_udp", 8192, NULL, 10, NULL, 0);
    BaseType_t discovery_created = xTaskCreate(
        discovery_task, "rgb_discovery", 4096, NULL, 6, NULL);
    ESP_ERROR_CHECK(display_created == pdPASS && stats_created == pdPASS &&
                            server_created == pdPASS &&
                            discovery_created == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}
