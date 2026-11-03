#include "mic_stream.h"

#include <limits.h>
#include <string.h>
#include "box2_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

enum
{
    // Capture block: 10 ms at 48 kHz, matching the I2S DMA granularity.
    BLOCK_SAMPLES = 480,
    BLOCK_BYTES = BLOCK_SAMPLES * sizeof(int16_t),
    RING48_BYTES = BLOCK_BYTES * 12,

    // One captured block becomes exactly 160 samples at 16 kHz.
    BLOCK16_SAMPLES = BLOCK_SAMPLES / 3,
    BLOCK16_BYTES = BLOCK16_SAMPLES * sizeof(int16_t),
    RING16_BYTES = BLOCK16_BYTES * 24,

    // Keep ~30 ms queued for USB: enough to ride out scheduling jitter, small
    // enough that push-to-talk latency stays imperceptible.
    TARGET48_BYTES = BLOCK_BYTES * 3,
    // Water level is corrected in 1 ms steps. A 10 ms block drop would be an
    // audible seam every few seconds; a single millisecond slip is not.
    TRIM_UNIT_BYTES = 48 * sizeof(int16_t),

    DC_FIXED_SCALE = 256,
    DC_FILTER_DIVISOR = 1024,
};

static const char *TAG = "mic_stream";

static RingbufHandle_t s_ring48;
static RingbufHandle_t s_ring16;
static TaskHandle_t s_task;

// Only the capture task touches these.
static int32_t s_dc_q8;
static volatile int s_peak;

static volatile uint32_t s_read_errors;

/*
 * 4th-order Butterworth low pass at 7 kHz, run at 48 kHz before decimating by
 * three. Measured response: flat to 4 kHz, -1 dB at 6 kHz, -6.6 dB at 8 kHz,
 * -43 dB at 16 kHz, so content that would fold back into the 0-8 kHz speech
 * band is already gone.
 */
typedef struct
{
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

static biquad_t s_lpf1 = {0.112881403f, 0.225762807f, 0.112881403f,
                          -0.702567175f, 0.154092788f, 0.0f, 0.0f};
static biquad_t s_lpf2 = {0.150060454f, 0.300120909f, 0.150060454f,
                          -0.933967389f, 0.534209207f, 0.0f, 0.0f};

static inline float biquad_step(biquad_t *f, float x)
{
    const float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static int16_t remove_dc(int16_t sample)
{
    const int32_t scaled = (int32_t)sample * DC_FIXED_SCALE;
    // A continuous first-order DC estimate, about 7.5 Hz at 48 kHz. The state
    // survives block boundaries so no discontinuity appears every 10 ms.
    s_dc_q8 += (scaled - s_dc_q8) / DC_FILTER_DIVISOR;
    int32_t centered = (scaled - s_dc_q8) / DC_FIXED_SCALE;
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

// Drops the oldest audio when a producer outruns its consumer. Without this
// the ring simply fills and every later block is lost.
static void drop_one_item(RingbufHandle_t ring, size_t max_bytes)
{
    size_t taken = 0;
    void *item = xRingbufferReceiveUpTo(ring, &taken, 0, max_bytes);
    if (item)
    {
        vRingbufferReturnItem(ring, item);
    }
}

static void push_block(RingbufHandle_t ring, const void *data, size_t bytes, size_t max_bytes)
{
    if (xRingbufferSend(ring, data, bytes, 0) == pdTRUE)
    {
        return;
    }
    drop_one_item(ring, max_bytes);
    if (xRingbufferSend(ring, data, bytes, 0) != pdTRUE)
    {
        // Still full: give up on this block rather than block the DMA reader.
        return;
    }
}

static void trim_48k(void)
{
    const size_t used = RING48_BYTES - xRingbufferGetCurFreeSize(s_ring48);
    if (used <= TARGET48_BYTES)
    {
        return;
    }
    size_t excess = used - TARGET48_BYTES;
    while (excess >= TRIM_UNIT_BYTES)
    {
        size_t taken = 0;
        void *item = xRingbufferReceiveUpTo(s_ring48, &taken, 0, TRIM_UNIT_BYTES);
        if (!item || taken == 0)
        {
            break;
        }
        vRingbufferReturnItem(s_ring48, item);
        excess -= taken;
    }
}

static void capture_task(void *argument)
{
    (void)argument;
    int16_t block[BLOCK_SAMPLES];
    int16_t block16[BLOCK16_SAMPLES];

    for (;;)
    {
        if (box2_audio_read(block, BLOCK_SAMPLES) != ESP_OK)
        {
            s_read_errors = s_read_errors + 1;
            // Resync the DC estimate after a gap instead of carrying a stale one.
            s_dc_q8 = 0;
            s_peak = 0;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        int peak = 0;
        for (size_t i = 0; i < BLOCK_SAMPLES; ++i)
        {
            const int16_t sample = remove_dc(block[i]);
            block[i] = sample;
            const int magnitude = sample < 0 ? -(int)sample : (int)sample;
            if (magnitude > peak)
            {
                peak = magnitude;
            }
        }
        s_peak = peak;
        push_block(s_ring48, block, BLOCK_BYTES, TRIM_UNIT_BYTES);

        for (size_t i = 0; i < BLOCK16_SAMPLES; ++i)
        {
            float filtered = 0.0f;
            // Three input samples produce one output sample.
            for (size_t k = 0; k < 3; ++k)
            {
                const size_t index = i * 3 + k;
                filtered = biquad_step(&s_lpf1, (float)block[index]);
                filtered = biquad_step(&s_lpf2, filtered);
            }
            int value = (int)filtered;
            if (value > INT16_MAX)
            {
                value = INT16_MAX;
            }
            else if (value < INT16_MIN)
            {
                value = INT16_MIN;
            }
            block16[i] = (int16_t)value;
        }
        push_block(s_ring16, block16, BLOCK16_BYTES, BLOCK16_BYTES);
    }
}

static size_t read_ring(RingbufHandle_t ring, int16_t *dst, size_t samples,
                        uint32_t timeout_ms)
{
    const size_t want = samples * sizeof(int16_t);
    size_t got = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (got < want)
    {
        TickType_t wait = 0;
        if (timeout_ms > 0)
        {
            const TickType_t now = xTaskGetTickCount();
            wait = deadline > now ? deadline - now : 0;
        }
        size_t taken = 0;
        void *item = xRingbufferReceiveUpTo(ring, &taken, wait, want - got);
        if (!item || taken == 0)
        {
            break;
        }
        memcpy((uint8_t *)dst + got, item, taken);
        vRingbufferReturnItem(ring, item);
        got += taken;
    }

    if (got < want && got >= sizeof(int16_t))
    {
        // Hold the last sample instead of dropping to zero: a DC step in the
        // middle of a word is far more damaging to ASR than a flat stretch.
        const int16_t last = dst[got / sizeof(int16_t) - 1];
        for (size_t i = got / sizeof(int16_t); i < samples; ++i)
        {
            dst[i] = last;
        }
        got = want;
    }
    return got / sizeof(int16_t);
}

esp_err_t mic_stream_start(void)
{
    if (s_task)
    {
        return ESP_OK;
    }
    if (box2_audio_sample_rate() != 48000)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_ring48 = xRingbufferCreate(RING48_BYTES, RINGBUF_TYPE_BYTEBUF);
    s_ring16 = xRingbufferCreate(RING16_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_ring48 || !s_ring16)
    {
        ESP_LOGE(TAG, "Cannot allocate capture rings");
        return ESP_ERR_NO_MEM;
    }
    // Capture is paced by the I2S DMA clock. It has to preempt the screen and
    // the wake engine, otherwise every long render costs an overrun block.
    // Pinned to core 1 so TinyUSB keeps core 0 to itself.
    if (xTaskCreatePinnedToCore(capture_task, "mic_capture", 4096, NULL, 14, &s_task, 1) != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Continuous capture: 48 kHz for USB, 16 kHz for the wake engine");
    return ESP_OK;
}

size_t mic_stream_read_pcm(int16_t *dst, size_t samples, uint32_t timeout_ms)
{
    if (!dst || !samples)
    {
        return 0;
    }
    const size_t got = read_ring(s_ring48, dst, samples, timeout_ms);
    trim_48k();
    return got;
}

void mic_stream_discard(void)
{
    if (!s_ring48)
    {
        return;
    }
    for (;;)
    {
        size_t taken = 0;
        void *item = xRingbufferReceiveUpTo(s_ring48, &taken, 0, RING48_BYTES);
        if (!item)
        {
            return;
        }
        vRingbufferReturnItem(s_ring48, item);
    }
}

size_t mic_stream_read_16k(int16_t *dst, size_t samples, uint32_t timeout_ms)
{
    if (!dst || !samples)
    {
        return 0;
    }
    return read_ring(s_ring16, dst, samples, timeout_ms);
}

int mic_stream_peak(void)
{
    return s_peak;
}

uint32_t mic_stream_read_errors(void)
{
    return s_read_errors;
}
