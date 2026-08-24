#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
typedef struct
{
    bool detected;
    uint8_t who_am_i;
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    int x_mg;
    int y_mg;
    int z_mg;
    const char *orientation;
} box2_motion_state_t;
esp_err_t box2_motion_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t box2_motion_read(box2_motion_state_t *state);
