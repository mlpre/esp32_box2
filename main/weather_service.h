#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum
{
    WEATHER_ICON_UNKNOWN = 0,
    WEATHER_ICON_CLEAR,
    WEATHER_ICON_PARTLY_CLOUDY,
    WEATHER_ICON_CLOUDY,
    WEATHER_ICON_FOG,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_THUNDER,
} weather_icon_t;

typedef struct
{
    bool valid;
    bool is_day;
    weather_icon_t icon;
    int temperature_c;
    char city[64];
} weather_status_t;

esp_err_t weather_service_start(void);
void weather_service_get_status(weather_status_t *status);
