#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "weather_service.h"

const lv_image_dsc_t *weather_icon_asset(weather_icon_t icon, bool is_day);
