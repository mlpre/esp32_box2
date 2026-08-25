#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "radio_stream.h"

typedef struct
{
    bool wifi_configured;
    bool wifi_connected;
    const char *wifi_ssid;
    const char *ip_address;
    int battery_percent;
    bool charging;
    radio_status_t radio;
} radio_screen_data_t;

esp_err_t radio_screen_show(const radio_screen_data_t *data);
