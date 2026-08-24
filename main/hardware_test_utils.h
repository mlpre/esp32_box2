#pragma once
#include <stdbool.h>
#include "box2_storage.h"
#include "esp_err.h"
typedef struct
{
    box2_storage_state_t storage;
    bool read_write_ok;
} hardware_test_storage_result_t;
esp_err_t hardware_test_audio_play_tone(int frequency_hz, int duration_ms);
esp_err_t hardware_test_audio_read_peak(int *peak);
esp_err_t hardware_test_storage(hardware_test_storage_result_t *result);
