#include "hardware_test_utils.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "box2_audio.h"
#include "esp_check.h"
static const char *TAG = "box2_test_utils";
esp_err_t hardware_test_audio_play_tone(int frequency_hz, int duration_ms)
{
    if (frequency_hz <= 0 || duration_ms <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    enum
    {
        FRAME_SAMPLES = 240
    };
    int16_t samples[FRAME_SAMPLES];
    uint32_t phase = 0;
    const uint32_t sample_rate = box2_audio_sample_rate();
    const uint32_t phase_step = ((uint64_t)(uint32_t)frequency_hz << 32) / sample_rate;
    int frames = (duration_ms * (int)sample_rate + FRAME_SAMPLES * 1000 - 1) /
                 (FRAME_SAMPLES * 1000);
    ESP_RETURN_ON_ERROR(box2_audio_set_output_volume(65), TAG, "enable test tone");
    for (int frame = 0; frame < frames; ++frame)
    {
        for (int i = 0; i < FRAME_SAMPLES; ++i)
        {
            uint16_t p = (uint16_t)(phase >> 16);
            int32_t triangle = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);
            samples[i] = (int16_t)((triangle - 16384) / 3);
            phase += phase_step;
        }
        esp_err_t err = box2_audio_write(samples, FRAME_SAMPLES);
        if (err != ESP_OK)
        {
            box2_audio_set_output_volume(0);
            return err;
        }
    }
    return box2_audio_set_output_volume(0);
}

esp_err_t hardware_test_audio_read_peak(int *peak)
{
    if (!peak)
    {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t samples[240];
    ESP_RETURN_ON_ERROR(box2_audio_read(samples, 240), TAG, "read microphone samples");
    int minimum = INT16_MAX;
    int maximum = INT16_MIN;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
    {
        if (samples[i] < minimum)
            minimum = samples[i];
        if (samples[i] > maximum)
            maximum = samples[i];
    }
    *peak = (maximum - minimum) / 2;
    return ESP_OK;
}

esp_err_t hardware_test_storage(hardware_test_storage_result_t *result)
{
    if (!result)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    esp_err_t err = box2_storage_mount(&result->storage);
    if (err != ESP_OK)
    {
        return err;
    }

    static const char expected[] = "BOX2 SD READ WRITE PASS\n";
    char path[64];
    snprintf(path, sizeof(path), "%s/box2_test.tmp", box2_storage_mount_point());
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return ESP_FAIL;
    }
    size_t written = fwrite(expected, 1, sizeof(expected), file);
    int close_error = fclose(file);
    if (written != sizeof(expected) || close_error != 0)
    {
        remove(path);
        return ESP_FAIL;
    }
    char actual[sizeof(expected)] = {0};
    file = fopen(path, "rb");
    if (!file)
    {
        remove(path);
        return ESP_FAIL;
    }
    size_t read = fread(actual, 1, sizeof(actual), file);
    close_error = fclose(file);
    int remove_error = remove(path);
    result->read_write_ok = read == sizeof(expected) && close_error == 0 &&
                            remove_error == 0 &&
                            memcmp(actual, expected, sizeof(expected)) == 0;
    return result->read_write_ok ? ESP_OK : ESP_FAIL;
}
