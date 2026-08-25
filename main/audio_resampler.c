#include "audio_resampler.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"

#define FIR_PHASE_COUNT 128
#define FIR_TAP_COUNT 32
#define FIR_LEFT_TAPS 15
#define FIR_RIGHT_TAPS 16
#define FIR_INPUT_CAPACITY 4096
#define FIR_OUTPUT_CAPACITY 1024
#define FIR_COEFFICIENT_SCALE 32768

struct audio_resampler
{
    uint32_t source_rate;
    uint32_t output_rate;
    uint64_t step_q32;
    uint64_t position_q32;
    int16_t *coefficients;
    int16_t *input;
    int16_t *output;
    size_t input_count;
    bool initialized;
};

static int16_t clamp_sample(int64_t sample)
{
    if (sample > INT16_MAX)
    {
        return INT16_MAX;
    }
    if (sample < INT16_MIN)
    {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static int16_t downmix_frame(const int16_t *pcm, size_t frame, uint8_t channels)
{
    if (channels == 1)
    {
        return pcm[frame];
    }
    int32_t mixed = (int32_t)pcm[frame * channels] + pcm[frame * channels + 1];
    return (int16_t)(mixed / 2);
}

static esp_err_t build_coefficients(audio_resampler_t *resampler,
                                    uint32_t source_rate)
{
    const double pi = 3.14159265358979323846;
    double cutoff = 0.96;
    if (source_rate > resampler->output_rate)
    {
        cutoff *= (double)resampler->output_rate / source_rate;
    }

    for (size_t phase = 0; phase < FIR_PHASE_COUNT; ++phase)
    {
        double fraction = (double)phase / FIR_PHASE_COUNT;
        double values[FIR_TAP_COUNT];
        double sum = 0.0;
        for (size_t tap = 0; tap < FIR_TAP_COUNT; ++tap)
        {
            double distance = (double)((int)tap - FIR_LEFT_TAPS) - fraction;
            double x = cutoff * distance;
            double sinc = fabs(x) < 1e-12 ? 1.0 : sin(pi * x) / (pi * x);
            double normalized_distance = distance / FIR_RIGHT_TAPS;
            double window = 0.0;
            if (fabs(normalized_distance) < 1.0)
            {
                window = 0.42 + 0.5 * cos(pi * normalized_distance) +
                         0.08 * cos(2.0 * pi * normalized_distance);
            }
            values[tap] = cutoff * sinc * window;
            sum += values[tap];
        }

        int coefficient_sum = 0;
        int16_t *phase_coefficients =
            resampler->coefficients + phase * FIR_TAP_COUNT;
        for (size_t tap = 0; tap < FIR_TAP_COUNT; ++tap)
        {
            long quantized = lround(values[tap] * FIR_COEFFICIENT_SCALE / sum);
            if (quantized > INT16_MAX)
            {
                quantized = INT16_MAX;
            }
            if (quantized < INT16_MIN)
            {
                quantized = INT16_MIN;
            }
            phase_coefficients[tap] = (int16_t)quantized;
            coefficient_sum += (int)quantized;
        }
        int correction = FIR_COEFFICIENT_SCALE - coefficient_sum;
        int corrected = phase_coefficients[FIR_LEFT_TAPS] + correction;
        if (corrected > INT16_MAX)
        {
            corrected = INT16_MAX;
        }
        if (corrected < INT16_MIN)
        {
            corrected = INT16_MIN;
        }
        phase_coefficients[FIR_LEFT_TAPS] = (int16_t)corrected;
    }
    return ESP_OK;
}

static esp_err_t configure(audio_resampler_t *resampler, uint32_t source_rate)
{
    ESP_RETURN_ON_FALSE(source_rate > 0 && resampler->output_rate > 0,
                        ESP_ERR_INVALID_ARG, "audio_resampler", "invalid rate");
    resampler->source_rate = source_rate;
    resampler->step_q32 = ((uint64_t)source_rate << 32) / resampler->output_rate;
    if (resampler->step_q32 == 0)
    {
        resampler->step_q32 = 1;
    }
    resampler->position_q32 = 0;
    resampler->input_count = 0;
    resampler->initialized = false;
    if (source_rate != resampler->output_rate)
    {
        return build_coefficients(resampler, source_rate);
    }
    return ESP_OK;
}

audio_resampler_t *audio_resampler_create(uint32_t output_rate)
{
    if (output_rate == 0)
    {
        return NULL;
    }
    audio_resampler_t *resampler = calloc(1, sizeof(*resampler));
    if (!resampler)
    {
        return NULL;
    }
    resampler->coefficients = heap_caps_malloc(
        FIR_PHASE_COUNT * FIR_TAP_COUNT * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    resampler->input = heap_caps_malloc(
        FIR_INPUT_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    resampler->output = heap_caps_malloc(
        FIR_OUTPUT_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!resampler->coefficients || !resampler->input || !resampler->output)
    {
        audio_resampler_destroy(resampler);
        return NULL;
    }
    resampler->output_rate = output_rate;
    return resampler;
}

void audio_resampler_destroy(audio_resampler_t *resampler)
{
    if (!resampler)
    {
        return;
    }
    free(resampler->coefficients);
    free(resampler->input);
    free(resampler->output);
    free(resampler);
}

static esp_err_t write_direct(audio_resampler_t *resampler,
                              const int16_t *pcm,
                              size_t frame_count,
                              uint8_t channels,
                              audio_resampler_write_cb_t write_cb,
                              void *write_context)
{
    size_t consumed = 0;
    while (consumed < frame_count)
    {
        size_t count = frame_count - consumed;
        if (count > FIR_OUTPUT_CAPACITY)
        {
            count = FIR_OUTPUT_CAPACITY;
        }
        for (size_t i = 0; i < count; ++i)
        {
            resampler->output[i] = downmix_frame(pcm, consumed + i, channels);
        }
        ESP_RETURN_ON_ERROR(write_cb(resampler->output, count, write_context),
                            "audio_resampler", "write direct audio");
        consumed += count;
    }
    return ESP_OK;
}

static esp_err_t produce_filtered(audio_resampler_t *resampler,
                                  audio_resampler_write_cb_t write_cb,
                                  void *write_context)
{
    size_t output_count = 0;
    while (true)
    {
        size_t center = (size_t)(resampler->position_q32 >> 32);
        if (center + FIR_RIGHT_TAPS >= resampler->input_count)
        {
            break;
        }
        uint32_t fraction = (uint32_t)resampler->position_q32;
        size_t phase = ((uint64_t)fraction * FIR_PHASE_COUNT) >> 32;
        const int16_t *coefficients =
            resampler->coefficients + phase * FIR_TAP_COUNT;
        const int16_t *input = resampler->input + center - FIR_LEFT_TAPS;
        int64_t accumulator = 0;
        for (size_t tap = 0; tap < FIR_TAP_COUNT; ++tap)
        {
            accumulator += (int32_t)input[tap] * coefficients[tap];
        }
        accumulator = accumulator >= 0
                          ? accumulator + FIR_COEFFICIENT_SCALE / 2
                          : accumulator - FIR_COEFFICIENT_SCALE / 2;
        resampler->output[output_count++] =
            clamp_sample(accumulator / FIR_COEFFICIENT_SCALE);
        resampler->position_q32 += resampler->step_q32;

        if (output_count == FIR_OUTPUT_CAPACITY)
        {
            ESP_RETURN_ON_ERROR(
                write_cb(resampler->output, output_count, write_context),
                "audio_resampler", "write filtered audio");
            output_count = 0;
        }
    }

    if (output_count)
    {
        ESP_RETURN_ON_ERROR(write_cb(resampler->output, output_count, write_context),
                            "audio_resampler", "write filtered audio tail");
    }

    size_t center = (size_t)(resampler->position_q32 >> 32);
    if (center > FIR_LEFT_TAPS)
    {
        size_t discard = center - FIR_LEFT_TAPS;
        memmove(resampler->input, resampler->input + discard,
                (resampler->input_count - discard) * sizeof(int16_t));
        resampler->input_count -= discard;
        resampler->position_q32 -= (uint64_t)discard << 32;
    }
    return ESP_OK;
}

esp_err_t audio_resampler_process(audio_resampler_t *resampler,
                                  const int16_t *pcm,
                                  size_t frame_count,
                                  uint8_t channels,
                                  uint32_t source_rate,
                                  audio_resampler_write_cb_t write_cb,
                                  void *write_context)
{
    ESP_RETURN_ON_FALSE(resampler && pcm && frame_count &&
                            (channels == 1 || channels == 2) && source_rate && write_cb,
                        ESP_ERR_INVALID_ARG, "audio_resampler", "invalid input");
    if (resampler->source_rate != source_rate)
    {
        ESP_RETURN_ON_ERROR(configure(resampler, source_rate),
                            "audio_resampler", "configure filter");
    }
    if (source_rate == resampler->output_rate)
    {
        return write_direct(resampler, pcm, frame_count, channels,
                            write_cb, write_context);
    }

    size_t consumed = 0;
    if (!resampler->initialized)
    {
        int16_t first = downmix_frame(pcm, 0, channels);
        for (size_t i = 0; i < FIR_LEFT_TAPS; ++i)
        {
            resampler->input[i] = first;
        }
        resampler->input_count = FIR_LEFT_TAPS;
        resampler->position_q32 = (uint64_t)FIR_LEFT_TAPS << 32;
        resampler->initialized = true;
    }

    while (consumed < frame_count)
    {
        size_t available = FIR_INPUT_CAPACITY - resampler->input_count;
        if (available == 0)
        {
            ESP_RETURN_ON_ERROR(produce_filtered(resampler, write_cb, write_context),
                                "audio_resampler", "drain input buffer");
            available = FIR_INPUT_CAPACITY - resampler->input_count;
            ESP_RETURN_ON_FALSE(available > 0, ESP_ERR_NO_MEM,
                                "audio_resampler", "resampler input stalled");
        }
        size_t copy_count = frame_count - consumed;
        if (copy_count > available)
        {
            copy_count = available;
        }
        for (size_t i = 0; i < copy_count; ++i)
        {
            resampler->input[resampler->input_count + i] =
                downmix_frame(pcm, consumed + i, channels);
        }
        resampler->input_count += copy_count;
        consumed += copy_count;
        ESP_RETURN_ON_ERROR(produce_filtered(resampler, write_cb, write_context),
                            "audio_resampler", "filter audio");
    }
    return ESP_OK;
}
