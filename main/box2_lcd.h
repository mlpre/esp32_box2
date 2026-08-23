#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t box2_lcd_init(void);
esp_err_t box2_lcd_show_color_test(void);
esp_err_t box2_lcd_show_lines(const char *title, const char *const *lines,
                              size_t line_count, int meter_percent);
