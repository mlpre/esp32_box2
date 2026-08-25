#include "radio_stream.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_resampler.h"
#include "box2_audio.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"

#define RADIO_TASK_STACK_SIZE (32 * 1024)
#define NETWORK_TASK_STACK_SIZE (10 * 1024)
#define PLAYBACK_TASK_STACK_SIZE (8 * 1024)
#define HTTP_READ_SIZE 4096
#define COMPRESSED_STREAM_BUFFER_SIZE (192 * 1024)
#define COMPRESSED_PREBUFFER_SIZE (48 * 1024)
#define COMPRESSED_DECODE_BUFFER_SIZE (32 * 1024)
#define PCM_STREAM_BUFFER_SIZE (192 * 1024)
#define PCM_PREBUFFER_SIZE (96 * 1024)
#define PCM_PLAYBACK_CHUNK_SIZE 4096
#define PCM_BUFFER_INITIAL_SIZE (16 * 1024)
#define RADIO_RETRY_DELAY_MS 2500
#define DIRECTORY_RETRY_DELAY_MS 5000
#define RADIO_OUTPUT_RATE 48000
#define DIRECTORY_MAX_STATIONS 1000
#define DIRECTORY_MAX_BYTES (256 * 1024)
#define DIRECTORY_CACHE_MAGIC 0x52414449UL
#define DIRECTORY_CACHE_VERSION 1UL
#define DIRECTORY_CACHE_SUBTYPE 0x40
#define DIRECTORY_CACHE_LABEL "radio_cache"
#define RADIO_NVS_NAMESPACE "radio"
#define RADIO_INDEX_KEY "station"
#define RADIO_VOLUME_KEY "volume"
#define DIRECTORY_URL                                                                            \
    "https://de1.api.radio-browser.info/m3u/stations/search?countrycode=CN&codec=MP3"            \
    "&hidebroken=true&order=votes&reverse=true&removeDuplicates=true&limit=1000"

typedef struct
{
    char name[128];
    char display_name[128];
    char url[256];
} radio_station_t;

#define PIPELINE_NETWORK_READY BIT0
#define PIPELINE_NETWORK_DONE BIT1
#define PIPELINE_NETWORK_FAILED BIT2
#define PIPELINE_DECODER_DONE BIT3
#define PIPELINE_PLAYBACK_DONE BIT4
#define PIPELINE_PLAYBACK_FAILED BIT5
#define PIPELINE_STOP_REQUESTED BIT6

typedef struct
{
    radio_station_t station;
    uint32_t generation;
    EventGroupHandle_t events;
    StreamBufferHandle_t compressed_stream;
    StreamBufferHandle_t pcm_stream;
    StaticStreamBuffer_t *compressed_control;
    StaticStreamBuffer_t *pcm_control;
    uint8_t *compressed_storage;
    uint8_t *pcm_storage;
    esp_err_t network_result;
    esp_err_t playback_result;
    char network_error[64];
    char playback_error[64];
} radio_pipeline_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    uint32_t checksum;
} directory_cache_header_t;

static const char *TAG = "radio_stream";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_status_t s_status = {
    .state = RADIO_STATE_IDLE,
    .station_index = 0,
    .station_count = 0,
    .volume_percent = 50,
};
static radio_station_t *s_active_stations;
static uint32_t s_generation = 1;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_directory_update_requested;

static void status_set_state(radio_state_t state, const char *error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    if (error)
    {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", error);
    }
    else
    {
        s_status.last_error[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

static void status_set_audio_info(const esp_audio_simple_dec_info_t *info)
{
    portENTER_CRITICAL(&s_lock);
    s_status.source_sample_rate = info->sample_rate;
    s_status.source_channels = info->channel;
    s_status.bitrate_kbps = info->bitrate / 1000;
    portEXIT_CRITICAL(&s_lock);
}

static void status_add_received(size_t byte_count)
{
    portENTER_CRITICAL(&s_lock);
    s_status.bytes_received += byte_count;
    portEXIT_CRITICAL(&s_lock);
}

static void get_selection(size_t *station_index, uint32_t *generation)
{
    portENTER_CRITICAL(&s_lock);
    *station_index = s_status.station_index;
    *generation = s_generation;
    portEXIT_CRITICAL(&s_lock);
}

static bool selection_changed(uint32_t generation)
{
    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = generation != s_generation;
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

static bool pipeline_stopping(const radio_pipeline_t *pipeline)
{
    return (xEventGroupGetBits(pipeline->events) & PIPELINE_STOP_REQUESTED) != 0 ||
           selection_changed(pipeline->generation);
}

static StreamBufferHandle_t create_psram_stream(size_t size,
                                                uint8_t **storage,
                                                StaticStreamBuffer_t **control)
{
    *storage = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *control = heap_caps_malloc(sizeof(**control),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!*storage || !*control)
    {
        free(*storage);
        free(*control);
        *storage = NULL;
        *control = NULL;
        return NULL;
    }
    StreamBufferHandle_t stream =
        xStreamBufferCreateStatic(size, 1, *storage, *control);
    if (!stream)
    {
        free(*storage);
        free(*control);
        *storage = NULL;
        *control = NULL;
    }
    return stream;
}

static void delete_psram_stream(StreamBufferHandle_t stream, uint8_t *storage,
                                StaticStreamBuffer_t *control)
{
    if (stream)
    {
        vStreamBufferDelete(stream);
    }
    free(storage);
    free(control);
}

static esp_err_t queue_pcm_samples(const int16_t *samples, size_t sample_count,
                                   void *context)
{
    radio_pipeline_t *pipeline = context;
    if (pipeline_stopping(pipeline))
    {
        return ESP_OK;
    }
    const uint8_t *data = (const uint8_t *)samples;
    size_t remaining = sample_count * sizeof(*samples);
    while (remaining && !pipeline_stopping(pipeline))
    {
        size_t sent = xStreamBufferSend(pipeline->pcm_stream, data, remaining,
                                        pdMS_TO_TICKS(100));
        data += sent;
        remaining -= sent;
    }
    return ESP_OK;
}

static esp_err_t render_decoded_frame(esp_audio_simple_dec_handle_t decoder,
                                      const uint8_t *pcm_data,
                                      size_t pcm_size,
                                      audio_resampler_t *resampler,
                                      radio_pipeline_t *pipeline)
{
    esp_audio_simple_dec_info_t info = {0};
    if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK)
    {
        return ESP_FAIL;
    }
    if (info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
        info.sample_rate == 0)
    {
        ESP_LOGE(TAG, "Unsupported PCM format: %" PRIu32 " Hz, %u bit, %u ch",
                 info.sample_rate, info.bits_per_sample, info.channel);
        return ESP_ERR_NOT_SUPPORTED;
    }
    radio_status_t current_status = {0};
    radio_stream_get_status(&current_status);
    if (current_status.source_sample_rate != info.sample_rate)
    {
        ESP_LOGI(TAG, "MP3 format: %" PRIu32 " Hz, %u ch, %" PRIu32 " kbps",
                 info.sample_rate, info.channel, info.bitrate / 1000);
    }
    status_set_audio_info(&info);

    size_t frame_count = pcm_size / (sizeof(int16_t) * info.channel);
    return audio_resampler_process(resampler, (const int16_t *)pcm_data,
                                   frame_count, info.channel, info.sample_rate,
                                   queue_pcm_samples, pipeline);
}

static void make_display_name(const char *source, char *destination, size_t capacity)
{
    if (!source || capacity == 0)
        return;

    size_t written = 0;
    const unsigned char *cursor = (const unsigned char *)source;
    while (*cursor)
    {
        size_t sequence_length = 1;
        if ((*cursor & 0xe0) == 0xc0)
            sequence_length = 2;
        else if ((*cursor & 0xf0) == 0xe0)
            sequence_length = 3;
        else if ((*cursor & 0xf8) == 0xf0)
            sequence_length = 4;

        bool valid = written + sequence_length < capacity;
        for (size_t i = 1; valid && i < sequence_length; ++i)
        {
            valid = cursor[i] && (cursor[i] & 0xc0) == 0x80;
        }
        if (!valid)
            break;
        memcpy(destination + written, cursor, sequence_length);
        written += sequence_length;
        cursor += sequence_length;
    }
    destination[written] = '\0';
}

static char *trim_line(char *line)
{
    while (*line == ' ' || *line == '\t' || *line == '\r')
    {
        ++line;
    }
    size_t length = strlen(line);
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' ||
                      line[length - 1] == '\r'))
    {
        line[--length] = '\0';
    }
    return line;
}

static size_t parse_m3u_directory(char *playlist, radio_station_t *stations, size_t capacity)
{
    size_t count = 0;
    const char *pending_name = NULL;
    char *save_pointer = NULL;
    for (char *line = strtok_r(playlist, "\n", &save_pointer); line && count < capacity;
         line = strtok_r(NULL, "\n", &save_pointer))
    {
        line = trim_line(line);
        if (strncmp(line, "#EXTINF:", 8) == 0)
        {
            char *comma = strchr(line, ',');
            pending_name = comma && comma[1] ? comma + 1 : "网络电台";
            continue;
        }
        if (line[0] == '#' || line[0] == '\0')
        {
            continue;
        }
        if (strncmp(line, "http://", 7) != 0 && strncmp(line, "https://", 8) != 0)
        {
            pending_name = NULL;
            continue;
        }
        if (strstr(line, ".m3u8"))
        {
            pending_name = NULL;
            continue;
        }
        if (strlen(line) >= sizeof(stations[count].url))
        {
            pending_name = NULL;
            continue;
        }

        snprintf(stations[count].name, sizeof(stations[count].name), "%s",
                 pending_name ? pending_name : "网络电台");
        make_display_name(stations[count].name, stations[count].display_name,
                          sizeof(stations[count].display_name));
        snprintf(stations[count].url, sizeof(stations[count].url), "%s", line);
        ++count;
        pending_name = NULL;
    }
    return count;
}

static uint32_t directory_checksum(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static esp_err_t save_station_index(size_t station_index)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RADIO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u32(handle, RADIO_INDEX_KEY, (uint32_t)station_index);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_volume(int volume_percent)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RADIO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(handle, RADIO_VOLUME_KEY, (uint8_t)volume_percent);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void load_volume(void)
{
    nvs_handle_t handle;
    uint8_t volume = 0;
    esp_err_t err = nvs_open(RADIO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        err = nvs_get_u8(handle, RADIO_VOLUME_KEY, &volume);
        nvs_close(handle);
    }
    if (err == ESP_OK && volume <= 100)
    {
        portENTER_CRITICAL(&s_lock);
        s_status.volume_percent = volume;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Loaded saved volume %u%%", (unsigned)volume);
    }
}

static void load_station_index(void)
{
    nvs_handle_t handle;
    uint32_t station_index = 0;
    esp_err_t err = nvs_open(RADIO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        err = nvs_get_u32(handle, RADIO_INDEX_KEY, &station_index);
        nvs_close(handle);
    }
    if (err == ESP_OK)
    {
        portENTER_CRITICAL(&s_lock);
        s_status.station_index = station_index;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Loaded saved station index %u", (unsigned)station_index);
    }
}

static const esp_partition_t *directory_cache_partition(void)
{
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)DIRECTORY_CACHE_SUBTYPE,
        DIRECTORY_CACHE_LABEL);
}

static esp_err_t save_station_directory(const uint8_t *playlist, size_t length)
{
    const esp_partition_t *partition = directory_cache_partition();
    if (!partition || !playlist || length == 0 || length > DIRECTORY_MAX_BYTES ||
        sizeof(directory_cache_header_t) + length > partition->size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    directory_cache_header_t header = {
        .magic = DIRECTORY_CACHE_MAGIC,
        .version = DIRECTORY_CACHE_VERSION,
        .length = (uint32_t)length,
        .checksum = directory_checksum(playlist, length),
    };
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(partition, 0, partition->size),
                        TAG, "erase station cache");
    ESP_RETURN_ON_ERROR(esp_partition_write(partition, 0, &header, sizeof(header)),
                        TAG, "write station cache header");
    return esp_partition_write(partition, sizeof(header), playlist, length);
}

static esp_err_t read_station_directory(uint8_t **playlist, size_t *length)
{
    const esp_partition_t *partition = directory_cache_partition();
    if (!partition || !playlist || !length)
    {
        return ESP_ERR_NOT_FOUND;
    }

    directory_cache_header_t header = {0};
    ESP_RETURN_ON_ERROR(esp_partition_read(partition, 0, &header, sizeof(header)),
                        TAG, "read station cache header");
    if (header.magic != DIRECTORY_CACHE_MAGIC ||
        header.version != DIRECTORY_CACHE_VERSION || header.length == 0 ||
        header.length > DIRECTORY_MAX_BYTES ||
        sizeof(header) + header.length > partition->size)
    {
        return ESP_ERR_INVALID_VERSION;
    }

    uint8_t *data = heap_caps_malloc(header.length + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_partition_read(partition, sizeof(header), data,
                                       header.length);
    if (err != ESP_OK || directory_checksum(data, header.length) != header.checksum)
    {
        free(data);
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }
    data[header.length] = '\0';
    *playlist = data;
    *length = header.length;
    return ESP_OK;
}

static esp_err_t install_station_directory(const uint8_t *playlist, size_t length,
                                           bool persist)
{
    if (!playlist || length == 0 || length > DIRECTORY_MAX_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char *parse_buffer = heap_caps_malloc(length + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radio_station_t *stations = heap_caps_calloc(
        DIRECTORY_MAX_STATIONS, sizeof(*stations),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!parse_buffer || !stations)
    {
        free(parse_buffer);
        free(stations);
        return ESP_ERR_NO_MEM;
    }
    memcpy(parse_buffer, playlist, length);
    parse_buffer[length] = '\0';
    size_t station_count = parse_m3u_directory(parse_buffer, stations,
                                               DIRECTORY_MAX_STATIONS);
    free(parse_buffer);
    if (station_count == 0)
    {
        free(stations);
        return ESP_ERR_NOT_FOUND;
    }

    if (persist)
    {
        esp_err_t cache_err = save_station_directory(playlist, length);
        if (cache_err != ESP_OK)
        {
            free(stations);
            return cache_err;
        }
    }

    bool reset_index = false;
    portENTER_CRITICAL(&s_lock);
    radio_station_t *old_stations = s_active_stations;
    s_active_stations = stations;
    s_status.station_count = station_count;
    if (persist)
    {
        s_status.station_index = 0;
        reset_index = true;
    }
    else if (s_status.station_index >= station_count)
    {
        s_status.station_index = 0;
        reset_index = true;
    }
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
    free(old_stations);
    if (reset_index)
    {
        esp_err_t index_err = save_station_index(0);
        if (index_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Station index reset was not saved: %s",
                     esp_err_to_name(index_err));
        }
    }
    ESP_LOGI(TAG, "Installed %u cached MP3 stations", (unsigned)station_count);
    return ESP_OK;
}

static esp_err_t load_initial_station_directory(void)
{
    uint8_t *cached_playlist = NULL;
    size_t cached_length = 0;
    esp_err_t cache_err = read_station_directory(&cached_playlist, &cached_length);
    if (cache_err == ESP_OK)
    {
        esp_err_t install_err = install_station_directory(
            cached_playlist, cached_length, false);
        free(cached_playlist);
        if (install_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Loaded station directory from Flash cache");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Cached station directory invalid: %s",
                 esp_err_to_name(install_err));
    }

    ESP_LOGI(TAG, "No station directory stored in Flash");
    return ESP_OK;
}

static esp_err_t load_station_directory(void)
{
    esp_err_t result = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    char *playlist = NULL;
    size_t received = 0;

    status_set_state(RADIO_STATE_LOADING_DIRECTORY, NULL);
    ESP_LOGI(TAG, "Loading up to %d MP3 stations from Radio Browser",
             DIRECTORY_MAX_STATIONS);

    esp_http_client_config_t config = {
        .url = DIRECTORY_URL,
        .user_agent = "BOX2-Internet-Radio/1.1",
        .timeout_ms = 15000,
        .buffer_size = HTTP_READ_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };
    client = esp_http_client_init(&config);
    if (!client)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    esp_http_client_set_header(client, "Accept", "audio/mpegurl,text/plain,*/*");
    result = esp_http_client_open(client, 0);
    if (result != ESP_OK || esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) != 200)
    {
        result = ESP_FAIL;
        goto cleanup;
    }

    playlist = heap_caps_malloc(DIRECTORY_MAX_BYTES + 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playlist)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (received < DIRECTORY_MAX_BYTES)
    {
        int read_size = esp_http_client_read(client, playlist + received,
                                             DIRECTORY_MAX_BYTES - received);
        if (read_size < 0)
        {
            result = ESP_FAIL;
            goto cleanup;
        }
        if (read_size == 0)
        {
            break;
        }
        received += (size_t)read_size;
    }
    playlist[received] = '\0';
    result = install_station_directory((const uint8_t *)playlist, received, true);
    if (result == ESP_OK)
    {
        radio_status_t status = {0};
        radio_stream_get_status(&status);
        ESP_LOGI(TAG, "Updated and saved %u MP3 stations (%u playlist bytes)",
                 (unsigned)status.station_count, (unsigned)received);
    }

cleanup:
    if (client)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(playlist);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Directory update failed (%s); keeping Flash cache",
                 esp_err_to_name(result));
        status_set_state(RADIO_STATE_RETRYING, "电台更新失败");
    }
    return result;
}

static void network_stream_task(void *argument)
{
    radio_pipeline_t *pipeline = argument;
    esp_http_client_handle_t client = NULL;
    uint8_t *buffer = NULL;
    esp_err_t result = ESP_FAIL;

    esp_http_client_config_t http_cfg = {
        .url = pipeline->station.url,
        .user_agent = "BOX2-Internet-Radio/1.2",
        .timeout_ms = 5000,
        .buffer_size = HTTP_READ_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    client = esp_http_client_init(&http_cfg);
    buffer = heap_caps_malloc(HTTP_READ_SIZE,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!client || !buffer)
    {
        snprintf(pipeline->network_error, sizeof(pipeline->network_error),
                 "网络缓冲内存不足");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    result = esp_http_client_open(client, 0);
    if (result != ESP_OK)
    {
        snprintf(pipeline->network_error, sizeof(pipeline->network_error),
                 "连接失败 %s", esp_err_to_name(result));
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0)
    {
        snprintf(pipeline->network_error, sizeof(pipeline->network_error),
                 "网络响应无效");
        result = ESP_FAIL;
        goto cleanup;
    }
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        snprintf(pipeline->network_error, sizeof(pipeline->network_error),
                 "网络状态码 %d", status_code);
        result = ESP_FAIL;
        goto cleanup;
    }

    xEventGroupSetBits(pipeline->events, PIPELINE_NETWORK_READY);
    if (!selection_changed(pipeline->generation))
    {
        status_set_state(RADIO_STATE_BUFFERING, NULL);
    }
    while (!pipeline_stopping(pipeline))
    {
        int read_size = esp_http_client_read(client, (char *)buffer, HTTP_READ_SIZE);
        if (read_size <= 0)
        {
            snprintf(pipeline->network_error, sizeof(pipeline->network_error),
                     "电台流已中断 %d", read_size);
            result = ESP_FAIL;
            break;
        }
        status_add_received((size_t)read_size);
        size_t sent = 0;
        while (sent < (size_t)read_size && !pipeline_stopping(pipeline))
        {
            sent += xStreamBufferSend(pipeline->compressed_stream, buffer + sent,
                                      (size_t)read_size - sent,
                                      pdMS_TO_TICKS(100));
        }
    }
    if (pipeline_stopping(pipeline))
    {
        result = ESP_OK;
        pipeline->network_error[0] = '\0';
    }

cleanup:
    if (client)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(buffer);
    pipeline->network_result = result;
    EventBits_t bits = PIPELINE_NETWORK_DONE;
    if (result != ESP_OK)
    {
        bits |= PIPELINE_NETWORK_FAILED;
    }
    xEventGroupSetBits(pipeline->events, bits);
    vTaskDelete(NULL);
}

static void audio_playback_task(void *argument)
{
    radio_pipeline_t *pipeline = argument;
    uint8_t *buffer = heap_caps_malloc(PCM_PLAYBACK_CHUNK_SIZE,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t result = buffer ? ESP_OK : ESP_ERR_NO_MEM;
    bool started = false;
    unsigned underflows = 0;
    if (!buffer)
    {
        snprintf(pipeline->playback_error, sizeof(pipeline->playback_error),
                 "播放缓冲内存不足");
        goto cleanup;
    }

    while (!pipeline_stopping(pipeline))
    {
        EventBits_t bits = xEventGroupGetBits(pipeline->events);
        size_t available = xStreamBufferBytesAvailable(pipeline->pcm_stream);
        if (!started && available < PCM_PREBUFFER_SIZE &&
            !(bits & PIPELINE_DECODER_DONE))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (available == 0 && (bits & PIPELINE_DECODER_DONE))
        {
            break;
        }

        size_t received = xStreamBufferReceive(
            pipeline->pcm_stream, buffer, PCM_PLAYBACK_CHUNK_SIZE,
            pdMS_TO_TICKS(200));
        if (received == 0)
        {
            if (started && !(xEventGroupGetBits(pipeline->events) &
                             PIPELINE_DECODER_DONE))
            {
                ++underflows;
                ESP_LOGW(TAG, "PCM buffer underflow %u", underflows);
            }
            continue;
        }
        if ((received & 1U) != 0)
        {
            snprintf(pipeline->playback_error, sizeof(pipeline->playback_error),
                     "播放数据未对齐");
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        result = box2_audio_write((const int16_t *)buffer,
                                  received / sizeof(int16_t));
        if (result != ESP_OK)
        {
            snprintf(pipeline->playback_error, sizeof(pipeline->playback_error),
                     "I2S输出失败 %s", esp_err_to_name(result));
            break;
        }
        if (!started)
        {
            started = true;
            status_set_state(RADIO_STATE_PLAYING, NULL);
            ESP_LOGI(TAG, "48 kHz playback started with %u buffered PCM bytes",
                     (unsigned)available);
        }
    }

cleanup:
    free(buffer);
    pipeline->playback_result = result;
    EventBits_t bits = PIPELINE_PLAYBACK_DONE;
    if (result != ESP_OK)
    {
        bits |= PIPELINE_PLAYBACK_FAILED;
    }
    xEventGroupSetBits(pipeline->events, bits);
    vTaskDelete(NULL);
}

static esp_err_t decode_pipeline(radio_pipeline_t *pipeline, char *error_text,
                                 size_t error_capacity)
{
    esp_err_t result = ESP_FAIL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    audio_resampler_t *resampler = NULL;
    uint8_t *compressed = NULL;
    uint8_t *pcm_buffer = NULL;
    size_t pcm_capacity = PCM_BUFFER_INITIAL_SIZE;
    size_t buffered = 0;

    esp_audio_simple_dec_cfg_t decoder_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&decoder_cfg, &decoder) != ESP_AUDIO_ERR_OK)
    {
        snprintf(error_text, error_capacity, "MP3解码器启动失败");
        result = ESP_FAIL;
        goto cleanup;
    }
    resampler = audio_resampler_create(RADIO_OUTPUT_RATE);
    compressed = heap_caps_malloc(COMPRESSED_DECODE_BUFFER_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcm_buffer = heap_caps_malloc(pcm_capacity,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resampler || !compressed || !pcm_buffer)
    {
        snprintf(error_text, error_capacity, "解码缓冲内存不足");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (!pipeline_stopping(pipeline))
    {
        EventBits_t bits = xEventGroupGetBits(pipeline->events);
        if (xStreamBufferBytesAvailable(pipeline->compressed_stream) >=
                COMPRESSED_PREBUFFER_SIZE ||
            (bits & PIPELINE_NETWORK_DONE))
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (!pipeline_stopping(pipeline))
    {
        size_t capacity = COMPRESSED_DECODE_BUFFER_SIZE - buffered;
        if (capacity)
        {
            buffered += xStreamBufferReceive(
                pipeline->compressed_stream, compressed + buffered, capacity,
                pdMS_TO_TICKS(100));
        }

        EventBits_t bits = xEventGroupGetBits(pipeline->events);
        if (buffered == 0)
        {
            if (bits & PIPELINE_NETWORK_DONE)
            {
                result = pipeline->network_result == ESP_OK
                             ? ESP_FAIL
                             : pipeline->network_result;
                if (pipeline->network_error[0])
                {
                    strlcpy(error_text, pipeline->network_error, error_capacity);
                }
                else
                {
                    snprintf(error_text, error_capacity, "电台流已结束");
                }
                break;
            }
            continue;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = compressed,
            .len = (uint32_t)buffered,
            .eos = false,
        };
        while (raw.len && !pipeline_stopping(pipeline))
        {
            esp_audio_simple_dec_out_t decoded = {
                .buffer = pcm_buffer,
                .len = (uint32_t)pcm_capacity,
            };
            raw.consumed = 0;
            esp_audio_err_t decode_err =
                esp_audio_simple_dec_process(decoder, &raw, &decoded);
            if (decode_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                uint8_t *larger = heap_caps_realloc(
                    pcm_buffer, decoded.needed_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!larger)
                {
                    snprintf(error_text, error_capacity, "解码内存不足");
                    result = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                pcm_buffer = larger;
                pcm_capacity = decoded.needed_size;
                continue;
            }
            if (decode_err != ESP_AUDIO_ERR_OK)
            {
                snprintf(error_text, error_capacity, "MP3解码错误 %d", decode_err);
                result = ESP_FAIL;
                goto cleanup;
            }
            if (decoded.decoded_size)
            {
                result = render_decoded_frame(decoder, decoded.buffer,
                                              decoded.decoded_size, resampler,
                                              pipeline);
                if (result != ESP_OK)
                {
                    if (pipeline_stopping(pipeline))
                    {
                        result = ESP_OK;
                    }
                    else
                    {
                        snprintf(error_text, error_capacity, "音频缓冲失败 %s",
                                 esp_err_to_name(result));
                    }
                    goto cleanup;
                }
            }
            if (raw.consumed > raw.len)
            {
                snprintf(error_text, error_capacity, "解码数据异常");
                result = ESP_FAIL;
                goto cleanup;
            }
            if (raw.consumed == 0)
            {
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }

        size_t remaining = raw.len;
        if (remaining == buffered && buffered == COMPRESSED_DECODE_BUFFER_SIZE)
        {
            snprintf(error_text, error_capacity, "MP3帧超过解码缓冲区");
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (remaining && raw.buffer != compressed)
        {
            memmove(compressed, raw.buffer, remaining);
        }
        buffered = remaining;
    }
    if (pipeline_stopping(pipeline))
    {
        result = ESP_OK;
    }

cleanup:
    if (decoder)
    {
        esp_audio_simple_dec_close(decoder);
    }
    audio_resampler_destroy(resampler);
    free(compressed);
    free(pcm_buffer);
    return result;
}

static esp_err_t play_station(size_t station_index, uint32_t generation)
{
    esp_err_t result = ESP_FAIL;
    char error_text[64] = {0};
    radio_pipeline_t pipeline = {
        .generation = generation,
        .network_result = ESP_FAIL,
        .playback_result = ESP_OK,
    };

    portENTER_CRITICAL(&s_lock);
    size_t station_count = s_status.station_count;
    if (station_count)
    {
        pipeline.station = s_active_stations[station_index % station_count];
    }
    portEXIT_CRITICAL(&s_lock);
    if (station_count == 0 || pipeline.station.url[0] == '\0')
    {
        return ESP_ERR_NOT_FOUND;
    }

    status_set_state(RADIO_STATE_CONNECTING, NULL);
    ESP_LOGI(TAG, "Opening station %u: %s", (unsigned)(station_index + 1),
             pipeline.station.name);

    pipeline.events = xEventGroupCreate();
    pipeline.compressed_stream = create_psram_stream(
        COMPRESSED_STREAM_BUFFER_SIZE, &pipeline.compressed_storage,
        &pipeline.compressed_control);
    pipeline.pcm_stream = create_psram_stream(
        PCM_STREAM_BUFFER_SIZE, &pipeline.pcm_storage, &pipeline.pcm_control);
    if (!pipeline.events || !pipeline.compressed_stream || !pipeline.pcm_stream)
    {
        snprintf(error_text, sizeof(error_text), "音频流水线内存不足");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    BaseType_t playback_created = xTaskCreatePinnedToCore(
        audio_playback_task, "radio_playback", PLAYBACK_TASK_STACK_SIZE,
        &pipeline, 8, NULL, 1);
    if (playback_created != pdPASS)
    {
        snprintf(error_text, sizeof(error_text), "播放任务启动失败");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    BaseType_t network_created = xTaskCreatePinnedToCore(
        network_stream_task, "radio_network", NETWORK_TASK_STACK_SIZE,
        &pipeline, 5, NULL, 0);
    if (network_created != pdPASS)
    {
        snprintf(error_text, sizeof(error_text), "网络任务启动失败");
        result = ESP_ERR_NO_MEM;
        xEventGroupSetBits(pipeline.events,
                           PIPELINE_STOP_REQUESTED | PIPELINE_DECODER_DONE);
        xEventGroupWaitBits(pipeline.events, PIPELINE_PLAYBACK_DONE, pdFALSE,
                            pdTRUE, portMAX_DELAY);
        goto cleanup;
    }

    result = decode_pipeline(&pipeline, error_text, sizeof(error_text));
    xEventGroupSetBits(pipeline.events,
                       PIPELINE_STOP_REQUESTED | PIPELINE_DECODER_DONE);
    xEventGroupWaitBits(pipeline.events,
                        PIPELINE_NETWORK_DONE | PIPELINE_PLAYBACK_DONE,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    if (result == ESP_OK && pipeline.playback_result != ESP_OK)
    {
        result = pipeline.playback_result;
        strlcpy(error_text, pipeline.playback_error, sizeof(error_text));
    }

cleanup:
    delete_psram_stream(pipeline.compressed_stream,
                        pipeline.compressed_storage,
                        pipeline.compressed_control);
    delete_psram_stream(pipeline.pcm_stream, pipeline.pcm_storage,
                        pipeline.pcm_control);
    if (pipeline.events)
    {
        vEventGroupDelete(pipeline.events);
    }
    if (!selection_changed(generation) && result != ESP_OK)
    {
        const char *message = error_text[0] ? error_text : esp_err_to_name(result);
        status_set_state(RADIO_STATE_RETRYING, message);
        ESP_LOGW(TAG, "%s; retrying", message);
    }
    return result;
}

static void radio_task(void *argument)
{
    (void)argument;
    uint32_t failed_generation = UINT32_MAX;
    unsigned consecutive_failures = 0;
    while (true)
    {
        bool update_directory;
        bool directory_missing;
        portENTER_CRITICAL(&s_lock);
        update_directory = s_directory_update_requested;
        s_directory_update_requested = false;
        directory_missing = s_status.station_count == 0;
        portEXIT_CRITICAL(&s_lock);
        if (update_directory || directory_missing)
        {
            esp_err_t directory_err = load_station_directory();
            if (directory_err != ESP_OK && directory_missing)
            {
                vTaskDelay(pdMS_TO_TICKS(DIRECTORY_RETRY_DELAY_MS));
                continue;
            }
        }

        size_t station_index;
        uint32_t generation;
        get_selection(&station_index, &generation);
        if (generation != failed_generation)
        {
            failed_generation = generation;
            consecutive_failures = 0;
        }
        esp_err_t result = play_station(station_index, generation);

        if (!selection_changed(generation) && result != ESP_OK &&
            ++consecutive_failures >= 2)
        {
            ESP_LOGW(TAG, "Station failed twice; advancing to the next station");
            radio_stream_select(station_index + 1);
            failed_generation = UINT32_MAX;
            consecutive_failures = 0;
            continue;
        }

        for (int elapsed = 0;
             elapsed < RADIO_RETRY_DELAY_MS && !selection_changed(generation);
             elapsed += 100)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t radio_stream_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK)
    {
        status_set_state(RADIO_STATE_ERROR, "MP3解码器初始化失败");
        return ESP_FAIL;
    }
    load_volume();
    ESP_RETURN_ON_ERROR(box2_audio_set_output_volume(s_status.volume_percent), TAG,
                        "set initial radio volume");
    load_station_index();
    ESP_RETURN_ON_ERROR(load_initial_station_directory(), TAG,
                        "load station directory from Flash");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t radio_stream_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task)
    {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreatePinnedToCore(
        radio_task, "radio_stream", RADIO_TASK_STACK_SIZE,
        NULL, 6, &s_task, 1);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void radio_stream_request_directory_update(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_directory_update_requested &&
        s_status.state != RADIO_STATE_LOADING_DIRECTORY)
    {
        s_directory_update_requested = true;
        s_status.state = RADIO_STATE_LOADING_DIRECTORY;
        s_status.last_error[0] = '\0';
        ++s_generation;
    }
    portEXIT_CRITICAL(&s_lock);
}

void radio_stream_select(size_t station_index)
{
    size_t selected_index;
    portENTER_CRITICAL(&s_lock);
    size_t count = s_status.station_count;
    s_status.station_index = count ? station_index % count : 0;
    selected_index = s_status.station_index;
    s_status.source_sample_rate = 0;
    s_status.source_channels = 0;
    s_status.bitrate_kbps = 0;
    s_status.bytes_received = 0;
    s_status.last_error[0] = '\0';
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = save_station_index(selected_index);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Station index was not saved: %s", esp_err_to_name(err));
    }
}

void radio_stream_next(void)
{
    radio_status_t status = {0};
    radio_stream_get_status(&status);
    radio_stream_select(status.station_index + 1);
}

void radio_stream_previous(void)
{
    radio_status_t status = {0};
    radio_stream_get_status(&status);
    if (status.station_count)
    {
        radio_stream_select((status.station_index + status.station_count - 1) %
                            status.station_count);
    }
}

esp_err_t radio_stream_set_volume(int volume_percent)
{
    if (volume_percent < 0)
        volume_percent = 0;
    if (volume_percent > 100)
        volume_percent = 100;
    esp_err_t err = box2_audio_set_output_volume(volume_percent);
    if (err == ESP_OK)
    {
        portENTER_CRITICAL(&s_lock);
        s_status.volume_percent = volume_percent;
        portEXIT_CRITICAL(&s_lock);
        esp_err_t save_err = save_volume(volume_percent);
        if (save_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Volume was not saved: %s",
                     esp_err_to_name(save_err));
        }
    }
    return err;
}

void radio_stream_get_status(radio_status_t *status)
{
    if (!status)
    {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void radio_stream_get_station_name(size_t station_index, char *name,
                                   size_t capacity)
{
    if (!name || capacity == 0)
    {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    size_t count = s_status.station_count;
    if (count)
    {
        strlcpy(name, s_active_stations[station_index % count].display_name,
                capacity);
    }
    else
    {
        strlcpy(name, "暂无电台", capacity);
    }
    portEXIT_CRITICAL(&s_lock);
}

const char *radio_stream_state_name(radio_state_t state)
{
    switch (state)
    {
    case RADIO_STATE_IDLE:
        return "待机";
    case RADIO_STATE_LOADING_DIRECTORY:
        return "正在更新电台";
    case RADIO_STATE_CONNECTING:
        return "正在连接";
    case RADIO_STATE_BUFFERING:
        return "正在缓冲";
    case RADIO_STATE_PLAYING:
        return "正在播放";
    case RADIO_STATE_RETRYING:
        return "正在重试";
    case RADIO_STATE_ERROR:
        return "发生错误";
    default:
        return "未知状态";
    }
}
