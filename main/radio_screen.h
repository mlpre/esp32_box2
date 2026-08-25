#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "radio_stream.h"
#include "weather_service.h"

typedef struct
{
    bool wifi_configured;
    bool wifi_connected;
    const char *wifi_ssid;
    const char *ip_address;
    int battery_percent;
    bool charging;
    radio_status_t radio;
    weather_status_t weather;
    uint16_t shutdown_minutes;
} radio_screen_data_t;

esp_err_t radio_screen_show(const radio_screen_data_t *data);
esp_err_t radio_screen_force_redraw(void);
