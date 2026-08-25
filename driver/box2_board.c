#include "box2_board.h"
#include "box2_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define TCA9555_REG_INPUT0 0x00
#define TCA9555_REG_OUTPUT0 0x02
#define TCA9555_REG_CONFIG0 0x06
static const char *TAG = "box2_board";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_tca9555;
static adc_oneshot_unit_handle_t s_adc;
static int s_i2c_device_count;
static bool s_key_l_idle;
static bool s_key_q_idle;
static bool s_key_m_idle;
static bool s_key_r_idle;
static esp_err_t tca_write_u16(uint8_t reg, uint16_t value)
{
    const uint8_t data[] = {reg, (uint8_t)value, (uint8_t)(value >> 8)};
    return i2c_master_transmit(s_tca9555, data, sizeof(data), 100);
}
static esp_err_t tca_read_u16(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_tca9555, &reg, 1, data, sizeof(data), 100);
    if (err == ESP_OK)
    {
        *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    }
    return err;
}
static int battery_percent_from_raw(int raw)
{
    static const struct
    {
        int raw;
        int percent;
    } levels[] = {
        {2696, 0},
        {2724, 20},
        {2861, 40},
        {3038, 60},
        {3150, 80},
        {3280, 100},
    };
    if (raw <= levels[0].raw)
    {
        return 0;
    }
    if (raw >= levels[5].raw)
    {
        return 100;
    }
    for (int i = 0; i < 5; ++i)
    {
        if (raw < levels[i + 1].raw)
        {
            return levels[i].percent +
                   (raw - levels[i].raw) * (levels[i + 1].percent - levels[i].percent) /
                       (levels[i + 1].raw - levels[i].raw);
        }
    }
    return 100;
}
static int battery_mv_from_raw(int raw)
{
    static const struct
    {
        int raw;
        int mv;
    } levels[] = {
        {2696, 3480},
        {2724, 3530},
        {2861, 3700},
        {3038, 3900},
        {3150, 4020},
        {3280, 4140},
    };
    if (raw <= levels[0].raw)
    {
        return levels[0].mv;
    }
    if (raw >= levels[5].raw)
    {
        return levels[5].mv;
    }
    for (int i = 0; i < 5; ++i)
    {
        if (raw < levels[i + 1].raw)
        {
            return levels[i].mv +
                   (raw - levels[i].raw) * (levels[i + 1].mv - levels[i].mv) /
                       (levels[i + 1].raw - levels[i].raw);
        }
    }
    return levels[5].mv;
}
static esp_err_t read_battery(int *raw)
{
    ESP_RETURN_ON_ERROR(tca_write_u16(TCA9555_REG_CONFIG0,
                                      BOX2_XIO_INPUT_MASK & ~BOX2_XIO_CHG_CTRL),
                        TAG, "enable battery divider");
    ESP_RETURN_ON_ERROR(tca_write_u16(TCA9555_REG_OUTPUT0,
                                      BOX2_XIO_SAFE_OUTPUTS & ~BOX2_XIO_CHG_CTRL),
                        TAG, "drive battery divider");
    vTaskDelay(pdMS_TO_TICKS(100));
    int total = 0;
    int sample = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 16; ++i)
    {
        err = adc_oneshot_read(s_adc, BOX2_BATTERY_ADC_CHANNEL, &sample);
        if (err != ESP_OK)
        {
            break;
        }
        total += sample;
    }
    esp_err_t restore_err = tca_write_u16(TCA9555_REG_CONFIG0, BOX2_XIO_INPUT_MASK);
    if (err == ESP_OK && restore_err == ESP_OK)
    {
        *raw = total / 16;
        return ESP_OK;
    }
    return err != ESP_OK ? err : restore_err;
}
esp_err_t box2_board_init(void)
{
    if (s_i2c_bus)
    {
        return ESP_OK;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOX2_I2C_SDA,
        .scl_io_num = BOX2_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "create I2C bus");
    const i2c_device_config_t tca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOX2_TCA9555_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &tca_cfg, &s_tca9555),
                        TAG, "add TCA9555");
    ESP_RETURN_ON_ERROR(tca_write_u16(TCA9555_REG_OUTPUT0, BOX2_XIO_SAFE_OUTPUTS),
                        TAG, "set safe expander outputs");
    ESP_RETURN_ON_ERROR(tca_write_u16(TCA9555_REG_CONFIG0, BOX2_XIO_INPUT_MASK),
                        TAG, "configure expander directions");
    s_i2c_device_count = 0;
    const gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << BOX2_BUTTON_RIGHT) | (1ULL << BOX2_TCA9555_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&key_cfg), TAG, "configure keys");
    /* These levels are fixed by the BOX2 key circuit. Runtime calibration is
       unsafe because a key held during deep-sleep wake would be learned as
       the idle state and invert that key until the next reboot. */
    s_key_l_idle = true;
    s_key_q_idle = true;
    s_key_m_idle = false;
    s_key_r_idle = true;
    ESP_LOGI(TAG, "key idle levels: L(P5)=%d Q(P6)=%d M(P7)=%d R(GPIO0)=%d",
             s_key_l_idle, s_key_q_idle, s_key_m_idle, s_key_r_idle);
    const adc_oneshot_unit_init_cfg_t adc_unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_unit_cfg, &s_adc), TAG, "create ADC");
    const adc_oneshot_chan_cfg_t adc_chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BOX2_BATTERY_ADC_CHANNEL,
                                                   &adc_chan_cfg),
                        TAG, "configure battery ADC");
    return ESP_OK;
}
i2c_master_bus_handle_t box2_board_i2c_bus(void)
{
    return s_i2c_bus;
}
int box2_board_i2c_device_count(void)
{
    return s_i2c_device_count;
}

esp_err_t box2_board_i2c_scan(uint8_t *addresses, size_t capacity, size_t *count)
{
    ESP_RETURN_ON_FALSE(s_i2c_bus && count, ESP_ERR_INVALID_ARG, TAG,
                        "invalid I2C scan arguments");
    size_t found = 0;
    for (uint8_t address = 1; address < 0x7f; ++address)
    {
        if (i2c_master_probe(s_i2c_bus, address, 20) == ESP_OK)
        {
            if (addresses && found < capacity)
            {
                addresses[found] = address;
            }
            ++found;
        }
    }
    *count = found;
    s_i2c_device_count = (int)found;
    return found > capacity && addresses ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
esp_err_t box2_board_read_state(box2_board_state_t *state, bool sample_battery)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is null");
    uint16_t xio = 0;
    ESP_RETURN_ON_ERROR(tca_read_u16(TCA9555_REG_INPUT0, &xio), TAG, "read expander");
    state->xio = xio;
    state->charging = (xio & BOX2_XIO_CHRG) == 0;
    state->left_level = (xio & BOX2_XIO_KEY_L) != 0;
    state->q_level = (xio & BOX2_XIO_KEY_Q) != 0;
    state->middle_level = (xio & BOX2_XIO_KEY_M) != 0;
    state->right_level = gpio_get_level(BOX2_BUTTON_RIGHT) != 0;
    state->left_pressed = state->left_level != s_key_l_idle;
    state->q_pressed = state->q_level != s_key_q_idle;
    state->middle_pressed = state->middle_level != s_key_m_idle;
    state->right_pressed = state->right_level != s_key_r_idle;
    state->expander_outputs_ok =
        (xio & BOX2_XIO_OUTPUT_MASK) == (BOX2_XIO_SAFE_OUTPUTS & BOX2_XIO_OUTPUT_MASK);
    if (sample_battery)
    {
        int raw = 0;
        ESP_RETURN_ON_ERROR(read_battery(&raw), TAG, "sample battery");
        state->battery_raw = raw;
        state->battery_percent = battery_percent_from_raw(raw);
        state->battery_mv_estimate = battery_mv_from_raw(raw);
    }
    return ESP_OK;
}

esp_err_t box2_board_power_off(void)
{
    ESP_RETURN_ON_FALSE(s_tca9555, ESP_ERR_INVALID_STATE, TAG,
                        "I/O expander is not initialized");
    /* Keep SYS_POW asserted. Dropping it while USB is attached power-cycles the
       board immediately; deep sleep below is the actual off state. */
    uint16_t outputs = BOX2_XIO_SAFE_OUTPUTS & ~BOX2_XIO_SPK_EN;
    ESP_RETURN_ON_ERROR(tca_write_u16(TCA9555_REG_OUTPUT0, outputs), TAG,
                        "disable speaker for standby");
    return ESP_OK;
}
