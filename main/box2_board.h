#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
typedef struct {
    uint16_t xio;
    int battery_raw;
    int battery_percent;
    int battery_mv_estimate;
    bool charging;
    bool left_pressed;
    bool q_pressed;
    bool middle_pressed;
    bool right_pressed;
    bool left_level;
    bool q_level;
    bool middle_level;
    bool right_level;
    bool expander_outputs_ok;
} box2_board_state_t;
esp_err_t box2_board_init(void);
i2c_master_bus_handle_t box2_board_i2c_bus(void);
int box2_board_i2c_device_count(void);
esp_err_t box2_board_read_state(box2_board_state_t *state, bool sample_battery);
esp_err_t box2_board_power_off(void);
