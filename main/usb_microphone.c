#include "usb_microphone.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "box2_audio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "usb_device_uac.h"
#include "usb_mode.h"

enum
{
    MIC_SAMPLE_RATE = 48000,
    MIC_FRAME_SAMPLES = MIC_SAMPLE_RATE / 1000 * CONFIG_UAC_MIC_INTERVAL_MS,
    MIC_FRAME_BYTES = MIC_FRAME_SAMPLES * sizeof(int16_t),
    DC_FIXED_SCALE = 256,
    DC_FILTER_DIVISOR = 1024,
};

_Static_assert(CONFIG_UAC_SAMPLE_RATE == MIC_SAMPLE_RATE, "USB and codec rates must be 48 kHz");
_Static_assert(CONFIG_UAC_MIC_CHANNEL_NUM == 1, "USB microphone must be mono");
_Static_assert(CONFIG_UAC_SPEAKER_CHANNEL_NUM == 0, "This module exports a microphone only");
_Static_assert(CONFIG_UAC_BIT_RESOLUTION == 16, "USB microphone must use 16-bit PCM");
_Static_assert(CONFIG_UAC_BYTES_PER_SAMPLE == sizeof(int16_t), "USB PCM slots must be 16-bit");
_Static_assert(CONFIG_UAC_MIC_INTERVAL_MS == 10, "USB microphone uses 10 ms capture blocks");

static const char *TAG = "usb_microphone";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_microphone_stats_t s_stats;
static bool s_local_muted;
static bool s_host_muted;
static bool s_powering_off;
static bool s_initialization_attempted;
static esp_err_t s_start_result = ESP_ERR_INVALID_STATE;

// Only the UAC microphone task accesses the capture/filter state.
static int16_t s_samples[MIC_FRAME_SAMPLES];
static int32_t s_dc_q8;
static bool s_dc_initialized;

static void microphone_mute_cb(uint32_t muted, void *cb_ctx)
{
    (void)cb_ctx;
    portENTER_CRITICAL(&s_state_lock);
    s_host_muted = muted != 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static void microphone_shutdown_handler(void)
{
    if (tud_inited())
    {
        (void)tud_disconnect();
    }
    // Shutdown handlers run before esp_restart suspends the scheduler.
    // Hold the detach long enough for the host to discard its UAC address.
    vTaskDelay(pdMS_TO_TICKS(200));
    // ESP32-S3 software restart does not reset the USB PHY selector.
    // Do not deinitialize TinyUSB here: the component's tud_task remains
    // alive until reboot and may still be waiting on its event queue.
    usb_mode_restore_serial_phy();
}

static int16_t remove_dc(int16_t sample)
{
    const int32_t scaled_sample = (int32_t)sample * DC_FIXED_SCALE;
    // A continuous first-order DC estimate, about 7.5 Hz at 48 kHz.
    // Keep the estimate across blocks to avoid block-boundary discontinuities.
    s_dc_q8 += (scaled_sample - s_dc_q8) / DC_FILTER_DIVISOR;
    int32_t centered = (scaled_sample - s_dc_q8) / DC_FIXED_SCALE;
    if (centered > INT16_MAX)
    {
        centered = INT16_MAX;
    }
    else if (centered < INT16_MIN)
    {
        centered = INT16_MIN;
    }
    return (int16_t)centered;
}

static esp_err_t microphone_input_cb(uint8_t *buf, size_t len, size_t *bytes_read,
                                     void *cb_ctx)
{
    (void)cb_ctx;
    if (!buf || !bytes_read)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes_read = 0;
    if (len != MIC_FRAME_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    portENTER_CRITICAL(&s_state_lock);
    const bool powering_off = s_powering_off;
    portEXIT_CRITICAL(&s_state_lock);
    if (powering_off)
    {
        // The upstream component has no stop/deinit API and can keep its
        // capture task active after a software detach. Do not touch the codec
        // again while the application waits for the physical power key.
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    // The component calls this from its microphone task, not the USB ISR.
    // Its task already schedules capture at 10 ms intervals. The I2S read
    // blocks for samples; adding a delay here would disrupt that schedule.
    memset(s_samples, 0, sizeof(s_samples));
    const esp_err_t read_result = box2_audio_read(s_samples, MIC_FRAME_SAMPLES);
    int peak = 0;
    if (read_result == ESP_OK)
    {
        if (!s_dc_initialized)
        {
            int32_t sum = 0;
            for (size_t i = 0; i < MIC_FRAME_SAMPLES; ++i)
            {
                sum += s_samples[i];
            }
            s_dc_q8 = (sum / MIC_FRAME_SAMPLES) * DC_FIXED_SCALE;
            s_dc_initialized = true;
        }
        for (size_t i = 0; i < MIC_FRAME_SAMPLES; ++i)
        {
            const int16_t sample = remove_dc(s_samples[i]);
            const int magnitude = sample < 0 ? -(int)sample : (int)sample;
            if (magnitude > peak)
            {
                peak = magnitude;
            }
            // Explicit USB little-endian encoding avoids buffer alignment
            // assumptions and preserves exactly one 16-bit sample per frame.
            const uint16_t encoded = (uint16_t)sample;
            buf[2 * i] = (uint8_t)encoded;
            buf[2 * i + 1] = (uint8_t)(encoded >> 8);
        }
    }
    else
    {
        // Drop the entire failed read, including any partial data. Return
        // silence successfully so UAC keeps its normal scheduling cadence.
        memset(buf, 0, len);
        s_dc_initialized = false;
    }

    portENTER_CRITICAL(&s_state_lock);
    const bool muted = s_local_muted || s_host_muted;
    s_stats.read_errors += read_result != ESP_OK;
    s_stats.bytes_sent += (uint32_t)len;
    s_stats.peak = peak;
    portEXIT_CRITICAL(&s_state_lock);
    if (muted)
    {
        // Capture and DC tracking continue while muted; no stale PCM is kept.
        memset(buf, 0, len);
    }
    *bytes_read = len;
    return ESP_OK;
}

esp_err_t usb_microphone_start(void)
{
    if (box2_audio_sample_rate() != MIC_SAMPLE_RATE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_initialization_attempted)
    {
        const esp_err_t result = s_start_result;
        portEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    // uac_device_init has no rollback/deinit API. Do not retry a partial
    // initialization in this boot: its internal tasks/PHY may already exist.
    s_initialization_attempted = true;
    portEXIT_CRITICAL(&s_state_lock);

    uac_device_config_t config = {
        .skip_tinyusb_init = false,
        .output_cb = NULL,
        .input_cb = microphone_input_cb,
        // The component's MIC-only descriptor uses feature-unit entity 2,
        // which its control handlers also call the speaker feature unit.
        .set_mute_cb = microphone_mute_cb,
        .set_volume_cb = NULL,
        .cb_ctx = NULL,
#if CONFIG_USB_DEVICE_UAC_AS_PART
        // The project descriptor has AC interface 0 and MIC AS interface 1.
        .spk_itf_num = -1,
        .mic_itf_num = 1,
#endif
    };
    esp_err_t result = esp_register_shutdown_handler(microphone_shutdown_handler);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "Starting USB microphone: 48 kHz, 16-bit, mono");
        result = uac_device_init(&config);
    }
    portENTER_CRITICAL(&s_state_lock);
    s_start_result = result;
    portEXIT_CRITICAL(&s_state_lock);
    return result;
}

void usb_microphone_set_muted(bool muted)
{
    portENTER_CRITICAL(&s_state_lock);
    s_local_muted = muted;
    portEXIT_CRITICAL(&s_state_lock);
}

void usb_microphone_prepare_power_off(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_powering_off = true;
    s_local_muted = true;
    s_stats.peak = 0;
    portEXIT_CRITICAL(&s_state_lock);
    if (tud_inited())
    {
        (void)tud_disconnect();
    }
    // Let an in-flight 10 ms capture complete and make the detach observable
    // before dropping the board power latch. Keep TinyUSB tasks/queues alive.
    vTaskDelay(pdMS_TO_TICKS(200));
}

bool usb_microphone_is_muted(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool muted = s_local_muted || s_host_muted;
    portEXIT_CRITICAL(&s_state_lock);
    return muted;
}

int usb_microphone_peak(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const int peak = s_stats.peak;
    portEXIT_CRITICAL(&s_state_lock);
    return peak;
}

void usb_microphone_get_stats(usb_microphone_stats_t *stats)
{
    if (!stats)
    {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_state_lock);
}
