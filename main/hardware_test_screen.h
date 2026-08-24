#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t hardware_test_screen_show_color_bars(void);
esp_err_t hardware_test_screen_show_lines(const char *title,
                                          const char *const *lines,
                                          size_t line_count,
                                          int meter_percent);
