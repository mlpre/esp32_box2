#include "weather_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define WEATHER_TASK_STACK_SIZE (10 * 1024)
#define WEATHER_RESPONSE_CAPACITY 4096
#define WEATHER_REFRESH_MS (10UL * 60UL * 1000UL)
#define LOCATION_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)
#define RETRY_DELAY_MS (60UL * 1000UL)
#define WEATHER_CACHE_MAGIC 0x57454154UL
#define WEATHER_CACHE_VERSION 1UL
#define WEATHER_NVS_NAMESPACE "weather"
#define WEATHER_NVS_KEY "current"
#define LOCATION_URL \
    "http://ipwho.is/?fields=success,city,latitude,longitude&lang=zh-CN"

typedef struct
{
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} http_response_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    weather_status_t status;
} weather_cache_t;

static const char *TAG = "weather_service";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_status_t s_status;
static TaskHandle_t s_task;

static esp_err_t save_cached_status(const weather_status_t *status)
{
    weather_cache_t cache = {
        .magic = WEATHER_CACHE_MAGIC,
        .version = WEATHER_CACHE_VERSION,
        .status = *status,
    };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(handle, WEATHER_NVS_KEY, &cache, sizeof(cache));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        return ESP_OK;
    }

    http_response_t *response = (http_response_t *)event->user_data;
    size_t incoming = (size_t)event->data_len;
    if (!response || response->length + incoming >= response->capacity)
    {
        if (response)
        {
            response->overflow = true;
        }
        return ESP_FAIL;
    }

    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_json(const char *url, char *buffer, size_t capacity)
{
    http_response_t response = {
        .data = buffer,
        .capacity = capacity,
    };
    buffer[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || response.overflow || response.length == 0)
    {
        ESP_LOGW(TAG, "HTTP request failed: err=%s status=%d bytes=%u",
                 esp_err_to_name(err), status, (unsigned)response.length);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_OK;
}

static esp_err_t fetch_location(char *buffer, size_t capacity, double *latitude,
                                double *longitude, char *city,
                                size_t city_capacity)
{
    esp_err_t err = fetch_json(LOCATION_URL, buffer, capacity);
    if (err != ESP_OK)
    {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    cJSON *city_value = cJSON_GetObjectItemCaseSensitive(root, "city");
    bool valid = cJSON_IsTrue(success) && cJSON_IsNumber(lat) &&
                 cJSON_IsNumber(lon) && cJSON_IsString(city_value) &&
                 city_value->valuestring && city_value->valuestring[0];
    if (valid)
    {
        *latitude = lat->valuedouble;
        *longitude = lon->valuedouble;
        strlcpy(city, city_value->valuestring, city_capacity);
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static weather_icon_t icon_for_wmo_code(int code)
{
    if (code == 0)
        return WEATHER_ICON_CLEAR;
    if (code == 1 || code == 2)
        return WEATHER_ICON_PARTLY_CLOUDY;
    if (code == 3)
        return WEATHER_ICON_CLOUDY;
    if (code == 45 || code == 48)
        return WEATHER_ICON_FOG;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
        return WEATHER_ICON_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return WEATHER_ICON_SNOW;
    if (code >= 95 && code <= 99)
        return WEATHER_ICON_THUNDER;
    return WEATHER_ICON_UNKNOWN;
}

static esp_err_t fetch_weather(char *buffer, size_t capacity, double latitude,
                               double longitude, const char *city,
                               weather_status_t *status)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
             "&current=temperature_2m,weather_code,is_day&timezone=auto",
             latitude, longitude);
    esp_err_t err = fetch_json(url, buffer, capacity);
    if (err != ESP_OK)
    {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *temperature = current ? cJSON_GetObjectItemCaseSensitive(
                                      current, "temperature_2m")
                                : NULL;
    cJSON *weather_code = current ? cJSON_GetObjectItemCaseSensitive(
                                       current, "weather_code")
                                  : NULL;
    cJSON *is_day = current ? cJSON_GetObjectItemCaseSensitive(current, "is_day")
                            : NULL;
    bool valid = cJSON_IsNumber(temperature) && cJSON_IsNumber(weather_code) &&
                 cJSON_IsNumber(is_day);
    if (valid)
    {
        double value = temperature->valuedouble;
        status->valid = true;
        status->is_day = is_day->valueint != 0;
        status->temperature_c = (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
        status->icon = icon_for_wmo_code(weather_code->valueint);
        strlcpy(status->city, city, sizeof(status->city));
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void publish_status(const weather_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    s_status = *status;
    portEXIT_CRITICAL(&s_lock);
}

static void weather_task(void *argument)
{
    (void)argument;
    char *buffer = heap_caps_malloc(WEATHER_RESPONSE_CAPACITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer)
    {
        ESP_LOGE(TAG, "Unable to allocate weather response buffer");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    bool have_location = false;
    double latitude = 0.0;
    double longitude = 0.0;
    char city[64] = "";
    TickType_t location_updated = 0;

    while (true)
    {
        TickType_t now = xTaskGetTickCount();
        if (!have_location ||
            (now - location_updated) >= pdMS_TO_TICKS(LOCATION_REFRESH_MS))
        {
            esp_err_t location_err = fetch_location(
                buffer, WEATHER_RESPONSE_CAPACITY, &latitude, &longitude,
                city, sizeof(city));
            if (location_err != ESP_OK)
            {
                ESP_LOGW(TAG, "IP location failed: %s",
                         esp_err_to_name(location_err));
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                continue;
            }
            have_location = true;
            location_updated = now;
            ESP_LOGI(TAG, "IP location resolved to %s", city);
        }

        weather_status_t next = {0};
        esp_err_t weather_err = fetch_weather(
            buffer, WEATHER_RESPONSE_CAPACITY, latitude, longitude, city, &next);
        if (weather_err == ESP_OK)
        {
            publish_status(&next);
            esp_err_t cache_err = save_cached_status(&next);
            if (cache_err != ESP_OK)
            {
                ESP_LOGW(TAG, "Weather cache write failed: %s",
                         esp_err_to_name(cache_err));
            }
            ESP_LOGI(TAG, "Weather updated: %s, %d C", next.city,
                     next.temperature_c);
            vTaskDelay(pdMS_TO_TICKS(WEATHER_REFRESH_MS));
        }
        else
        {
            ESP_LOGW(TAG, "Weather update failed: %s",
                     esp_err_to_name(weather_err));
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
    }
}

esp_err_t weather_service_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }

    weather_cache_t cache = {0};
    size_t size = sizeof(cache);
    err = nvs_get_blob(handle, WEATHER_NVS_KEY, &cache, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }
    if (size != sizeof(cache) || cache.magic != WEATHER_CACHE_MAGIC ||
        cache.version != WEATHER_CACHE_VERSION || !cache.status.valid)
    {
        return ESP_ERR_INVALID_VERSION;
    }

    publish_status(&cache.status);
    ESP_LOGI(TAG, "Loaded cached weather: %s, %d C", cache.status.city,
             cache.status.temperature_c);
    return ESP_OK;
}

esp_err_t weather_service_start(void)
{
    if (s_task)
    {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(weather_task, "weather", WEATHER_TASK_STACK_SIZE,
                                     NULL, 3, &s_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void weather_service_get_status(weather_status_t *status)
{
    if (!status)
    {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
