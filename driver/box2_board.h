#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
typedef struct
{
    uint16_t xio;
    int battery_raw;
    int battery_percent;
    int battery_mv_estimate;
    bool charging;
    // Physical order: q (back arrow), left (left arrow), middle (M), right (right arrow).
    bool q_pressed;
    bool left_pressed;
    bool middle_pressed;
    bool right_pressed;
    bool q_level;
    bool left_level;
    bool middle_level;
    bool right_level;
    bool expander_outputs_ok;
} box2_board_state_t;
esp_err_t box2_board_init(void);
i2c_master_bus_handle_t box2_board_i2c_bus(void);
int box2_board_i2c_device_count(void);
esp_err_t box2_board_i2c_scan(uint8_t *addresses, size_t capacity, size_t *count);
/**
 * Enable or disable the speaker amplifier (TCA9555 SPK_EN, active high).
 * Preserves USB_SEL and the power outputs. Battery measurements preserve this
 * setting as well. Requires successful board initialization; may briefly wait
 * for an ongoing battery measurement.
 */
esp_err_t box2_board_set_speaker_enabled(bool enabled);
/**
 * Release the board's power latch using the BOX2 reference power-off sequence.
 * Disables the speaker, holds CHG_CTRL low as an output, waits 100 ms, then
 * clears SYS_POW. Preserves USB_SEL, the peripheral power rails and key inputs.
 * The caller must first stop audio/USB activity and blank the display.
 * On battery power this can remove power before the function returns. With
 * external power the MCU may remain running; the caller must provide standby
 * and M-button restart handling. This API does not enter a sleep mode.
 * Battery sampling is blocked until power_on(), including after an I2C error.
 */
esp_err_t box2_board_power_off(void);
/**
 * Reassert SYS_POW and restore CHG_CTRL to an input after software standby.
 * Restores battery sampling only after both writes succeed. Leaves the
 * speaker disabled; callers normally restart to initialize their saved mode.
 */
esp_err_t box2_board_power_on(void);
/**
 * Read board inputs. During power-off/standby, sample_battery must be false:
 * otherwise returns ESP_ERR_INVALID_STATE without changing power outputs.
 */
esp_err_t box2_board_read_state(box2_board_state_t *state, bool sample_battery);
