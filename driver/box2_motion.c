#include "box2_motion.h"
#include <stdlib.h>
#include "box2_config.h"
#include "esp_check.h"
#include "esp_log.h"
#define SC7A20_REG_WHO_AM_I 0x0F
#define SC7A20_REG_CTRL1 0x20
#define SC7A20_REG_CTRL2 0x21
#define SC7A20_REG_CTRL3 0x22
#define SC7A20_REG_OUT_X_L 0x28
#define SC7A20_REG_MISC 0x57
#define SC7A20_WHO_AM_I 0x11
static const char *TAG = "box2_motion";
static i2c_master_dev_handle_t s_sensor;
static uint8_t s_who_am_i;
static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_sensor, data, sizeof(data), 100);
}
static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_sensor, &reg, 1, value, 1, 100);
}
static const char *orientation_from_mg(int x, int y, int z)
{
    int ax = abs(x);
    int ay = abs(y);
    int az = abs(z);
    if (az > ax && az > ay && az > 650)
    {
        return z >= 0 ? "FACE UP" : "FACE DOWN";
    }
    if (ax > ay && ax > 650)
    {
        return x >= 0 ? "+X" : "-X";
    }
    if (ay > 650)
    {
        return y >= 0 ? "+Y" : "-Y";
    }
    return "MOVING";
}
esp_err_t box2_motion_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_sensor)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_ARG, TAG, "I2C bus is null");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOX2_SC7A20_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &config, &s_sensor),
                        TAG, "add SC7A20");
    ESP_RETURN_ON_ERROR(read_reg(SC7A20_REG_WHO_AM_I, &s_who_am_i), TAG,
                        "read SC7A20 ID");
    ESP_RETURN_ON_FALSE(s_who_am_i == SC7A20_WHO_AM_I, ESP_ERR_NOT_FOUND, TAG,
                        "unexpected SC7A20 ID 0x%02X", s_who_am_i);
    ESP_RETURN_ON_ERROR(write_reg(SC7A20_REG_CTRL1, 0x47), TAG, "configure CTRL1");
    ESP_RETURN_ON_ERROR(write_reg(SC7A20_REG_CTRL2, 0x07), TAG, "configure CTRL2");
    ESP_RETURN_ON_ERROR(write_reg(SC7A20_REG_CTRL3, 0x00), TAG, "configure CTRL3");
    ESP_RETURN_ON_ERROR(write_reg(SC7A20_REG_MISC, 0x08), TAG, "configure sensor");
    ESP_LOGI(TAG, "SC7A20 PASS: address=0x%02X ID=0x%02X", BOX2_SC7A20_ADDR,
             s_who_am_i);
    return ESP_OK;
}
esp_err_t box2_motion_read(box2_motion_state_t *state)
{
    ESP_RETURN_ON_FALSE(state && s_sensor, ESP_ERR_INVALID_STATE, TAG,
                        "SC7A20 is not initialized");
    uint8_t data[6];
    for (int i = 0; i < 6; ++i)
    {
        ESP_RETURN_ON_ERROR(read_reg(SC7A20_REG_OUT_X_L + i, &data[i]), TAG,
                            "read acceleration");
    }
    state->detected = true;
    state->who_am_i = s_who_am_i;
    state->raw_x = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    state->raw_y = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    state->raw_z = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
    state->x_mg = (int32_t)state->raw_x * 2000 / 32768;
    state->y_mg = (int32_t)state->raw_y * 2000 / 32768;
    state->z_mg = (int32_t)state->raw_z * 2000 / 32768;
    state->orientation = orientation_from_mg(state->x_mg, state->y_mg, state->z_mg);
    return ESP_OK;
}
