#pragma once
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t box2_audio_set_output_volume(int volume_percent);
esp_err_t box2_audio_set_input_gain(float gain_db);
esp_err_t box2_audio_write(const int16_t *samples, size_t sample_count);
esp_err_t box2_audio_read(int16_t *samples, size_t sample_count);
uint32_t box2_audio_sample_rate(void);
