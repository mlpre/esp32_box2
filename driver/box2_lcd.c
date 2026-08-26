#include "box2_lcd.h"
#include <stdbool.h>
#include <stdint.h>
#include "box2_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "box2_lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static SemaphoreHandle_t s_transfer_done;
static bool IRAM_ATTR on_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                             esp_lcd_panel_io_event_data_t *edata,
                                             void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t lcd_command(uint8_t command, const uint8_t *data, size_t length)
{
    return esp_lcd_panel_io_tx_param(s_panel_io, command, data, length);
}

esp_err_t box2_lcd_init(void)
{
    if (s_panel)
    {
        return ESP_OK;
    }

    const gpio_config_t rd_cfg = {
        .pin_bit_mask = 1ULL << BOX2_LCD_RD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rd_cfg), TAG, "configure LCD RD");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOX2_LCD_RD, 1), TAG, "idle LCD RD");

    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = BOX2_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG,
                        "configure backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG,
                        "configure backlight PWM");

    const esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = BOX2_LCD_DC,
        .wr_gpio_num = BOX2_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            BOX2_LCD_D0,
            BOX2_LCD_D1,
            BOX2_LCD_D2,
            BOX2_LCD_D3,
            BOX2_LCD_D4,
            BOX2_LCD_D5,
            BOX2_LCD_D6,
            BOX2_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = BOX2_LCD_WIDTH * BOX2_LCD_HEIGHT * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    esp_lcd_i80_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG,
                        "create LCD I80 bus");

    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done, ESP_ERR_NO_MEM, TAG,
                        "create LCD semaphore");
    xSemaphoreGive(s_transfer_done);
    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = BOX2_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_transfer_done,
        .user_ctx = s_transfer_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 1,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .cs_active_high = 0,
            .reverse_color_bits = 0,
            // RGB565 buffers use the CPU-native little-endian layout. The
            // 8-bit panel bus must put the high byte on the wire first.
            .swap_color_bytes = 1,
            .pclk_active_neg = 0,
            .pclk_idle_low = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_panel_io),
                        TAG, "create LCD panel IO");
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel),
                        TAG, "create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "initialize panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG,
                        "invert panel color");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "set panel gap");

    static const uint8_t cf[] = {0x00, 0x83, 0x30};
    static const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
    static const uint8_t e8[] = {0x85, 0x01, 0x79};
    static const uint8_t cb[] = {0x39, 0x2c, 0x00, 0x34, 0x02};
    static const uint8_t e0[] = {
        0xd0,
        0x00,
        0x02,
        0x07,
        0x0a,
        0x28,
        0x32,
        0x44,
        0x42,
        0x06,
        0x0e,
        0x12,
        0x14,
        0x17,
    };
    static const uint8_t e1[] = {
        0xd0,
        0x00,
        0x02,
        0x07,
        0x0a,
        0x28,
        0x31,
        0x54,
        0x47,
        0x0e,
        0x1c,
        0x17,
        0x1b,
        0x1e,
    };
    static const struct
    {
        uint8_t cmd;
        uint8_t value;
    } middle_sequence[] = {
        {0xbb, 0x20},
        {0xc3, 0x00},
        {0xc4, 0x20},
        {0xc5, 0x20},
        {0xc6, 0x10},
        {0xc7, 0xb0},
        {0x36, 0x60},
        {0x3a, 0x55},
    };
    const uint8_t ea[] = {0x00, 0x00};
    const uint8_t b1[] = {0x00, 0x1b};
    const uint8_t f7 = 0x20;
    const uint8_t f2 = 0x08;
    const uint8_t gamma = 0x01;
    const uint8_t b7 = 0x07;
    ESP_RETURN_ON_ERROR(lcd_command(0xcf, cf, sizeof(cf)), TAG, "LCD sequence CF");
    ESP_RETURN_ON_ERROR(lcd_command(0xed, ed, sizeof(ed)), TAG, "LCD sequence ED");
    ESP_RETURN_ON_ERROR(lcd_command(0xe8, e8, sizeof(e8)), TAG, "LCD sequence E8");
    ESP_RETURN_ON_ERROR(lcd_command(0xcb, cb, sizeof(cb)), TAG, "LCD sequence CB");
    ESP_RETURN_ON_ERROR(lcd_command(0xf7, &f7, 1), TAG, "LCD sequence F7");
    ESP_RETURN_ON_ERROR(lcd_command(0xea, ea, sizeof(ea)), TAG, "LCD sequence EA");
    for (size_t i = 0; i < sizeof(middle_sequence) / sizeof(middle_sequence[0]); ++i)
    {
        ESP_RETURN_ON_ERROR(lcd_command(middle_sequence[i].cmd,
                                        &middle_sequence[i].value, 1),
                            TAG, "LCD one-byte sequence");
    }
    ESP_RETURN_ON_ERROR(lcd_command(0xb1, b1, sizeof(b1)), TAG, "LCD sequence B1");
    ESP_RETURN_ON_ERROR(lcd_command(0xf2, &f2, 1), TAG, "LCD sequence F2");
    ESP_RETURN_ON_ERROR(lcd_command(0x26, &gamma, 1), TAG, "LCD gamma select");
    ESP_RETURN_ON_ERROR(lcd_command(0xe0, e0, sizeof(e0)), TAG, "LCD gamma E0");
    ESP_RETURN_ON_ERROR(lcd_command(0xe1, e1, sizeof(e1)), TAG, "LCD gamma E1");
    ESP_RETURN_ON_ERROR(lcd_command(0xb7, &b7, 1), TAG, "LCD sequence B7");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG,
                        "enable landscape LCD XY swap");
    // XY swap alone is a diagonal reflection. Combining it with X mirroring
    // produces a true 90-degree landscape rotation with readable text.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG,
                        "rotate LCD to readable landscape orientation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                        "turn display on");
    return box2_lcd_set_backlight(100);
}

esp_err_t box2_lcd_set_backlight(uint8_t percent)
{
    if (percent > 100)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty),
                        TAG, "set backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t box2_lcd_set_power(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG,
                        "LCD is not initialized");
    return esp_lcd_panel_disp_on_off(s_panel, enabled);
}

esp_err_t box2_lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                               const uint16_t *pixels)
{
    ESP_RETURN_ON_FALSE(s_panel && pixels, ESP_ERR_INVALID_STATE, TAG,
                        "LCD is not initialized");
    ESP_RETURN_ON_FALSE(x_start >= 0 && y_start >= 0 && x_end > x_start &&
                            y_end > y_start && x_end <= BOX2_LCD_WIDTH &&
                            y_end <= BOX2_LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid bitmap bounds");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "wait for LCD bus");
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start,
                                              x_end, y_end, pixels);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_transfer_done);
        return err;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "wait for LCD transfer");
    xSemaphoreGive(s_transfer_done);
    return ESP_OK;
}

int box2_lcd_width(void)
{
    return BOX2_LCD_WIDTH;
}

int box2_lcd_height(void)
{
    return BOX2_LCD_HEIGHT;
}
