#include "voice_wake.h"

#include <stdlib.h>
#include <string.h>
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_stream.h"
#include "model_path.h"

enum
{
    // 2 s of silence ends a dictation. Below ~1.2 s normal thinking pauses get
    // cut off mid-sentence; above ~3 s the user wonders whether it stopped.
    SILENCE_STOP_MS = 2000,
    // Nothing said at all after a wake or a key press: stop instead of
    // holding the host's dictation open indefinitely.
    NO_SPEECH_STOP_MS = 6000,
    // Cue tones and the tail of the wake word are ignored for this long.
    BLANK_MS = 700,
    FEED_TIMEOUT_MS = 200,
};

static const char *TAG = "voice_wake";

// ESP-IDF v6 requires an explicit spinlock for critical sections.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static TaskHandle_t s_task;
static int s_feed_samples;
static volatile bool s_available;

// Written by the main loop, read by the wake task.
static volatile bool s_listening;
static volatile uint32_t s_events;
static volatile uint32_t s_blank_until;

// Owned by the wake task.
static uint32_t s_listen_started;
static uint32_t s_last_speech;
static bool s_heard_speech;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void voice_wake_set_listening(bool listening)
{
    if (listening != s_listening)
    {
        // The main loop is the authority: it also reacts to the button, which
        // the detector never sees.
        s_listening = listening;
        s_listen_started = now_ms();
        s_last_speech = s_listen_started;
        s_heard_speech = false;
    }
}

void voice_wake_blank(uint32_t ms)
{
    s_blank_until = now_ms() + ms;
}

uint32_t voice_wake_take_events(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t events = s_events;
    s_events = 0;
    portEXIT_CRITICAL(&s_lock);
    return events;
}

bool voice_wake_available(void)
{
    return s_available;
}

static void emit(uint32_t event)
{
    portENTER_CRITICAL(&s_lock);
    s_events |= event;
    portEXIT_CRITICAL(&s_lock);
}

static void handle_result(const afe_fetch_result_t *result)
{
    const uint32_t now = now_ms();
    if ((int32_t)(now - s_blank_until) < 0)
    {
        return;
    }
    const bool speech = result->vad_state == VAD_SPEECH;

    if (!s_listening)
    {
        if (result->wakeup_state == WAKENET_DETECTED)
        {
            // Take ownership optimistically so a second detection arriving
            // before the main loop polls cannot queue two starts.
            s_listening = true;
            s_listen_started = now;
            s_last_speech = now;
            s_heard_speech = false;
            s_blank_until = now + BLANK_MS;
            emit(VOICE_WAKE_START);
            ESP_LOGI(TAG, "Wake word detected");
        }
        return;
    }

    if (speech)
    {
        s_last_speech = now;
        s_heard_speech = true;
        return;
    }

    const uint32_t quiet_for = now - (s_heard_speech ? s_last_speech : s_listen_started);
    const uint32_t limit = s_heard_speech ? SILENCE_STOP_MS : NO_SPEECH_STOP_MS;
    if (quiet_for >= limit)
    {
        s_listening = false;
        s_blank_until = now + BLANK_MS;
        emit(VOICE_WAKE_STOP);
        ESP_LOGI(TAG, "End of speech after %u ms", (unsigned)quiet_for);
    }
}

static void wake_task(void *argument)
{
    (void)argument;
    int16_t *buffer = malloc((size_t)s_feed_samples * sizeof(int16_t));
    if (!buffer)
    {
        ESP_LOGE(TAG, "No memory for the feed buffer");
        s_available = false;
        vTaskDelete(NULL);
        return;
    }

    while (true)
    {
        // Pads with the last sample on a short read, so the frame length the
        // front end expects is always exact.
        (void)mic_stream_read_16k(buffer, (size_t)s_feed_samples, FEED_TIMEOUT_MS);
        s_afe->feed(s_afe_data, buffer);
        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (result && result->data_size > 0)
        {
            handle_result(result);
        }
    }
}

esp_err_t voice_wake_start(void)
{
    if (s_task)
    {
        return ESP_OK;
    }
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models)
    {
        ESP_LOGE(TAG, "No model partition; wake word disabled");
        return ESP_ERR_NOT_FOUND;
    }
    if (!esp_srmodel_filter(models, ESP_WN_PREFIX, NULL))
    {
        ESP_LOGE(TAG, "No wake word model in the partition");
        return ESP_ERR_NOT_FOUND;
    }

    // Single microphone, so the input format is just "M".
    afe_config_t *config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!config)
    {
        ESP_LOGE(TAG, "AFE configuration rejected");
        return ESP_ERR_INVALID_ARG;
    }
    s_afe = esp_afe_handle_from_config(config);
    if (!s_afe)
    {
        ESP_LOGE(TAG, "No AFE implementation for this configuration");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_afe_data = s_afe->create_from_config(config);
    if (!s_afe_data)
    {
        ESP_LOGE(TAG, "AFE allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_feed_samples = s_afe->get_feed_chunksize(s_afe_data);
    if (s_feed_samples <= 0)
    {
        ESP_LOGE(TAG, "Invalid feed size");
        return ESP_ERR_INVALID_STATE;
    }

    s_blank_until = now_ms() + BLANK_MS;
    // Lower priority than capture: a late feed frame costs detection latency,
    // a late I2S read costs samples that are gone forever.
    if (xTaskCreatePinnedToCore(wake_task, "voice_wake", 8192, NULL, 6, &s_task, 1) != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_available = true;
    ESP_LOGI(TAG, "Wake word ready, %d samples per feed", s_feed_samples);
    return ESP_OK;
}
