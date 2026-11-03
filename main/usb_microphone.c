#include "usb_microphone.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "box2_audio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_stream.h"
#include "sdkconfig.h"
#include "tusb.h"
#include "usb_device_uac.h"
#include "usb_mode.h"

enum
{
    MIC_SAMPLE_RATE = 48000,
    MIC_FRAME_SAMPLES = MIC_SAMPLE_RATE / 1000 * CONFIG_UAC_MIC_INTERVAL_MS,
    MIC_FRAME_BYTES = MIC_FRAME_SAMPLES * sizeof(int16_t),
    // Long enough to ride out a scheduling hiccup, short enough that the
    // host never gets stale audio after the mute gate opens.
    MIC_READ_TIMEOUT_MS = 10,
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

// Only the UAC microphone task accesses this staging buffer. DC removal and
// peak tracking live in mic_stream, which owns the I2S reader.
static int16_t s_samples[MIC_FRAME_SAMPLES];

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

    portENTER_CRITICAL(&s_state_lock);
    const bool muted = s_local_muted || s_host_muted;
    portEXIT_CRITICAL(&s_state_lock);

    // The component calls this from its microphone task, not the USB ISR, once
    // every 10 ms. Capture itself now runs continuously in mic_stream; this
    // callback only drains the ring at the host's cadence.
    memset(s_samples, 0, sizeof(s_samples));
    if (muted)
    {
        // While the gate is closed, drop what queued up. Otherwise opening the
        // gate would hand the host up to 100 ms of room audio recorded before
        // the user asked to talk.
        mic_stream_discard();
        memset(buf, 0, len);
    }
    else if (mic_stream_read_pcm(s_samples, MIC_FRAME_SAMPLES, MIC_READ_TIMEOUT_MS) ==
             MIC_FRAME_SAMPLES)
    {
        for (size_t i = 0; i < MIC_FRAME_SAMPLES; ++i)
        {
            // Explicit USB little-endian encoding avoids buffer alignment
            // assumptions and preserves exactly one 16-bit sample per frame.
            const uint16_t encoded = (uint16_t)s_samples[i];
            buf[2 * i] = (uint8_t)encoded;
            buf[2 * i + 1] = (uint8_t)(encoded >> 8);
        }
    }
    else
    {
        // Short read: return silence so UAC keeps its normal scheduling cadence.
        memset(buf, 0, len);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stats.read_errors = mic_stream_read_errors();
    s_stats.bytes_sent += (uint32_t)len;
    s_stats.peak = mic_stream_peak();
    portEXIT_CRITICAL(&s_state_lock);
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
