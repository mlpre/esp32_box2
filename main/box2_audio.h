#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t box2_audio_play_tone(int frequency_hz, int duration_ms);
esp_err_t box2_audio_read_peak(int *peak);
