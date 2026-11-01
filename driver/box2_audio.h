#pragma once
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
/** Initialize 16-bit mono audio at the default 24 kHz sample rate. */
esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus);
/**
 * Initialize 16-bit mono audio at 24 kHz or 48 kHz (USB microphone).
 * Repeated initialization at the current rate succeeds. A different rate returns
 * ESP_ERR_INVALID_STATE; change the operating mode by restarting the device.
 * Call from startup before creating audio reader/writer tasks.
 */
esp_err_t box2_audio_init_with_sample_rate(i2c_master_bus_handle_t i2c_bus,
                                           uint32_t sample_rate);
esp_err_t box2_audio_set_output_volume(int volume_percent);
esp_err_t box2_audio_set_input_gain(float gain_db);
esp_err_t box2_audio_write(const int16_t *samples, size_t sample_count);
esp_err_t box2_audio_read(int16_t *samples, size_t sample_count);
/** Return the initialized sample rate, or zero before successful initialization. */
uint32_t box2_audio_sample_rate(void);
