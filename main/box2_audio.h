#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    BOX2_SFX_MENU = 0,
    BOX2_SFX_START,
    BOX2_SFX_PASS,
    BOX2_SFX_CRASH,
    BOX2_SFX_GAME_OVER,
} box2_audio_effect_t;

esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t box2_audio_set_volume(int percent);
int box2_audio_get_volume(void);
void box2_audio_set_engine(int speed_percent);
esp_err_t box2_audio_play_effect(box2_audio_effect_t effect);
esp_err_t box2_audio_play_tone(int frequency_hz, int duration_ms);
esp_err_t box2_audio_read_peak(int *peak);
