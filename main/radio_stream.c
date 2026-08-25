#include "radio_stream.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "freertos/task.h"
#include "nvs.h"

#define RADIO_TASK_STACK_SIZE (32 * 1024)
#define HTTP_READ_SIZE 4096
#define HTTP_AUDIO_BUFFER_SIZE (16 * 1024)
#define HTTP_AUDIO_PREBUFFER_SIZE (12 * 1024)
#define PCM_BUFFER_INITIAL_SIZE 8192
#define RESAMPLE_OUTPUT_SAMPLES 4096
#define RADIO_RETRY_DELAY_MS 2500
#define DIRECTORY_RETRY_DELAY_MS 5000
#define RADIO_OUTPUT_RATE 24000
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

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    uint32_t checksum;
} directory_cache_header_t;

typedef struct
{
    uint32_t source_rate;
    bool started;
    int16_t previous;
    uint64_t next_q32;
    uint64_t step_q32;
} linear_resampler_t;

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

static esp_err_t write_output_samples(const int16_t *samples, size_t count)
{
    if (count == 0)
    {
        return ESP_OK;
    }
    return box2_audio_write(samples, count);
}

static void resampler_reset(linear_resampler_t *resampler, uint32_t source_rate)
{
    memset(resampler, 0, sizeof(*resampler));
    resampler->source_rate = source_rate;
    resampler->step_q32 = ((uint64_t)source_rate << 32) / RADIO_OUTPUT_RATE;
    if (resampler->step_q32 == 0)
    {
        resampler->step_q32 = 1;
    }
}

static esp_err_t resample_and_write(linear_resampler_t *resampler,
                                    const int16_t *pcm,
                                    size_t frame_count,
                                    uint8_t channels,
                                    int16_t *output,
                                    size_t output_capacity)
{
    const uint64_t one_q32 = 1ULL << 32;
    size_t output_count = 0;

    for (size_t i = 0; i < frame_count; ++i)
    {
        int16_t current;
        if (channels == 1)
        {
            current = pcm[i];
        }
        else
        {
            int32_t mixed = (int32_t)pcm[i * channels] + pcm[i * channels + 1];
            current = (int16_t)(mixed / 2);
        }

        if (!resampler->started)
        {
            resampler->started = true;
            resampler->previous = current;
            resampler->next_q32 = resampler->step_q32;
            output[output_count++] = current;
            continue;
        }

        while (resampler->next_q32 <= one_q32)
        {
            int32_t delta = (int32_t)current - resampler->previous;
            int32_t interpolated = resampler->previous +
                                   (int32_t)(((int64_t)delta *
                                              (int64_t)resampler->next_q32) >> 32);
            output[output_count++] = (int16_t)interpolated;
            resampler->next_q32 += resampler->step_q32;
            if (output_count == output_capacity)
            {
                ESP_RETURN_ON_ERROR(write_output_samples(output, output_count), TAG,
                                    "write resampled audio");
                output_count = 0;
            }
        }
        resampler->next_q32 -= one_q32;
        resampler->previous = current;
    }

    return write_output_samples(output, output_count);
}

static esp_err_t render_decoded_frame(esp_audio_simple_dec_handle_t decoder,
                                      const uint8_t *pcm_data,
                                      size_t pcm_size,
                                      linear_resampler_t *resampler,
                                      int16_t *resample_output,
                                      size_t resample_capacity)
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
    if (resampler->source_rate != info.sample_rate)
    {
        ESP_LOGI(TAG, "MP3 format: %" PRIu32 " Hz, %u ch, %" PRIu32 " kbps",
                 info.sample_rate, info.channel, info.bitrate / 1000);
        resampler_reset(resampler, info.sample_rate);
    }
    status_set_audio_info(&info);

    size_t frame_count = pcm_size / (sizeof(int16_t) * info.channel);
    return resample_and_write(resampler, (const int16_t *)pcm_data, frame_count,
                              info.channel, resample_output, resample_capacity);
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

static esp_err_t play_station(size_t station_index, uint32_t generation)
{
    esp_err_t result = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *http_buffer = NULL;
    uint8_t *pcm_buffer = NULL;
    int16_t *resample_output = NULL;
    size_t pcm_capacity = PCM_BUFFER_INITIAL_SIZE;
    linear_resampler_t resampler = {0};
    char error_text[64] = {0};
    radio_station_t station = {0};

    portENTER_CRITICAL(&s_lock);
    size_t station_count = s_status.station_count;
    if (station_count)
    {
        station = s_active_stations[station_index % station_count];
    }
    portEXIT_CRITICAL(&s_lock);
    if (station_count == 0 || station.url[0] == '\0')
    {
        return ESP_ERR_NOT_FOUND;
    }

    status_set_state(RADIO_STATE_CONNECTING, NULL);
    ESP_LOGI(TAG, "Opening station %u: %s", (unsigned)(station_index + 1),
             station.name);

    esp_http_client_config_t http_cfg = {
        .url = station.url,
        .user_agent = "BOX2-Internet-Radio/1.0",
        .timeout_ms = 5000,
        .buffer_size = HTTP_READ_SIZE,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    client = esp_http_client_init(&http_cfg);
    if (!client)
    {
        snprintf(error_text, sizeof(error_text), "网络模块启动失败");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    result = esp_http_client_open(client, 0);
    if (result != ESP_OK)
    {
        snprintf(error_text, sizeof(error_text), "连接失败 %s", esp_err_to_name(result));
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0)
    {
        snprintf(error_text, sizeof(error_text), "网络响应无效");
        result = ESP_FAIL;
        goto cleanup;
    }
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        snprintf(error_text, sizeof(error_text), "网络状态码 %d", status_code);
        result = ESP_FAIL;
        goto cleanup;
    }

    esp_audio_simple_dec_cfg_t decoder_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&decoder_cfg, &decoder) != ESP_AUDIO_ERR_OK)
    {
        snprintf(error_text, sizeof(error_text), "MP3解码器启动失败");
        result = ESP_FAIL;
        goto cleanup;
    }

    http_buffer = heap_caps_malloc(HTTP_AUDIO_BUFFER_SIZE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcm_buffer = malloc(pcm_capacity);
    resample_output = malloc(RESAMPLE_OUTPUT_SAMPLES * sizeof(int16_t));
    if (!http_buffer || !pcm_buffer || !resample_output)
    {
        snprintf(error_text, sizeof(error_text), "音频内存不足");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    status_set_state(RADIO_STATE_BUFFERING, NULL);
    size_t buffered = 0;
    while (buffered < HTTP_AUDIO_PREBUFFER_SIZE &&
           !selection_changed(generation))
    {
        int read_size = esp_http_client_read(
            client, (char *)http_buffer + buffered,
            HTTP_AUDIO_BUFFER_SIZE - buffered);
        if (read_size <= 0)
        {
            snprintf(error_text, sizeof(error_text),
                     "电台预缓冲失败 %d", read_size);
            result = ESP_FAIL;
            goto cleanup;
        }
        buffered += (size_t)read_size;
        status_add_received((size_t)read_size);
    }

    bool first_audio_frame = true;
    while (!selection_changed(generation))
    {
        if (buffered < HTTP_AUDIO_PREBUFFER_SIZE)
        {
            int read_size = esp_http_client_read(
                client, (char *)http_buffer + buffered,
                HTTP_AUDIO_BUFFER_SIZE - buffered);
            if (read_size <= 0)
            {
                snprintf(error_text, sizeof(error_text),
                         "电台流已中断 %d", read_size);
                result = ESP_FAIL;
                break;
            }
            buffered += (size_t)read_size;
            status_add_received((size_t)read_size);
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = http_buffer,
            .len = (uint32_t)buffered,
            .eos = false,
        };
        while (raw.len && !selection_changed(generation))
        {
            esp_audio_simple_dec_out_t decoded = {
                .buffer = pcm_buffer,
                .len = (uint32_t)pcm_capacity,
            };
            raw.consumed = 0;
            esp_audio_err_t decode_err = esp_audio_simple_dec_process(decoder, &raw, &decoded);
            if (decode_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                uint8_t *larger = realloc(pcm_buffer, decoded.needed_size);
                if (!larger)
                {
                    snprintf(error_text, sizeof(error_text), "解码内存不足");
                    result = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                pcm_buffer = larger;
                pcm_capacity = decoded.needed_size;
                continue;
            }
            if (decode_err != ESP_AUDIO_ERR_OK)
            {
                snprintf(error_text, sizeof(error_text), "MP3解码错误 %d", decode_err);
                result = ESP_FAIL;
                goto cleanup;
            }
            if (decoded.decoded_size)
            {
                result = render_decoded_frame(decoder, decoded.buffer, decoded.decoded_size,
                                              &resampler, resample_output,
                                              RESAMPLE_OUTPUT_SAMPLES);
                if (result != ESP_OK)
                {
                    snprintf(error_text, sizeof(error_text), "音频输出失败 %s",
                             esp_err_to_name(result));
                    goto cleanup;
                }
                if (first_audio_frame)
                {
                    first_audio_frame = false;
                    status_set_state(RADIO_STATE_PLAYING, NULL);
                }
            }
            if (raw.consumed > raw.len)
            {
                snprintf(error_text, sizeof(error_text), "解码数据异常");
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
        if (remaining == buffered)
        {
            if (buffered == HTTP_AUDIO_BUFFER_SIZE)
            {
                snprintf(error_text, sizeof(error_text), "MP3帧超过缓冲区");
                result = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
        }
        else if (remaining)
        {
            memmove(http_buffer, raw.buffer, remaining);
        }
        buffered = remaining;
    }

cleanup:
    if (decoder)
    {
        esp_audio_simple_dec_close(decoder);
    }
    if (client)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(http_buffer);
    free(pcm_buffer);
    free(resample_output);

    if (!selection_changed(generation))
    {
        status_set_state(RADIO_STATE_RETRYING,
                         error_text[0] ? error_text : esp_err_to_name(result));
        ESP_LOGW(TAG, "%s; retrying", error_text[0] ? error_text : esp_err_to_name(result));
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
    BaseType_t created = xTaskCreate(radio_task, "radio_stream", RADIO_TASK_STACK_SIZE,
                                     NULL, 6, &s_task);
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
