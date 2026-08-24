#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t box2_lcd_init(void);
esp_err_t box2_lcd_set_backlight(uint8_t percent);
esp_err_t box2_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                               const uint16_t *pixels);
int box2_lcd_width(void);
int box2_lcd_height(void);
