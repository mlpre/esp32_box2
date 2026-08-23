#include "box2_lcd.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "box2_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static const char *TAG = "box2_lcd";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static SemaphoreHandle_t s_frame_done;
static uint16_t *s_framebuffer;
static bool s_dashboard_valid;
static char s_last_title[32];
static char s_last_lines[17][40];
static size_t s_last_line_count;
static int s_last_meter = -1;
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                                ((uint16_t)(g & 0xfc) << 3) | (b >> 3));
    return (uint16_t)((color << 8) | (color >> 8));
}
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
static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > BOX2_LCD_WIDTH) {
        width = BOX2_LCD_WIDTH - x;
    }
    if (y + height > BOX2_LCD_HEIGHT) {
        height = BOX2_LCD_HEIGHT - y;
    }
    for (int row = y; row < y + height; ++row) {
        uint16_t *pixel = s_framebuffer + row * BOX2_LCD_WIDTH + x;
        for (int col = 0; col < width; ++col) {
            pixel[col] = color;
        }
    }
}
static const uint8_t *glyph(char c)
{
    static const uint8_t digits[10][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
        {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
    };
    static const uint8_t letters[26][5] = {
        {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
        {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
        {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
        {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
        {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
        {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
        {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
        {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
        {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
        {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    };
    static const uint8_t space[5] = {0};
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t dot[5] = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t colon[5] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t percent[5] = {0x62,0x64,0x08,0x13,0x23};
    static const uint8_t equal[5] = {0x14,0x14,0x14,0x14,0x14};
    static const uint8_t hash[5] = {0x14,0x7f,0x14,0x7f,0x14};
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }
    switch (c) {
        case '-': return dash;
        case '.': return dot;
        case ':': return colon;
        case '/': return slash;
        case '%': return percent;
        case '=': return equal;
        case '#': return hash;
        default: return space;
    }
}
static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *bitmap = glyph(c);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((bitmap[col] & (1U << row)) != 0) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}
static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    while (*text && x + 5 * scale < BOX2_LCD_WIDTH) {
        draw_char(x, y, *text++, scale, color);
        x += 6 * scale;
    }
}
static esp_err_t begin_frame(uint16_t background)
{
    if (xSemaphoreTake(s_frame_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    fill_rect(0, 0, BOX2_LCD_WIDTH, BOX2_LCD_HEIGHT, background);
    return ESP_OK;
}
static esp_err_t present_frame(void)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOX2_LCD_WIDTH,
                                              BOX2_LCD_HEIGHT, s_framebuffer);
    if (err != ESP_OK) {
        xSemaphoreGive(s_frame_done);
    }
    return err;
}
static esp_err_t present_region(int y_start, int y_end)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y_start, BOX2_LCD_WIDTH,
                                              y_end, s_framebuffer + y_start * BOX2_LCD_WIDTH);
    if (err != ESP_OK) {
        xSemaphoreGive(s_frame_done);
    }
    return err;
}
static void draw_meter(int meter_percent)
{
    fill_rect(0, 282, BOX2_LCD_WIDTH, 38, rgb565(5, 10, 20));
    draw_text(6, 284, "MIC", 1, rgb565(150, 170, 190));
    fill_rect(30, 283, 204, 11, rgb565(25, 35, 50));
    fill_rect(32, 285, meter_percent * 200 / 100, 7,
              meter_percent > 85 ? rgb565(255,70,50) : rgb565(40,210,100));
    draw_text(6, 301, "LIVE AUDIO INPUT LEVEL", 1, rgb565(100,190,230));
}
static esp_err_t lcd_command(uint8_t command, const uint8_t *data, size_t length)
{
    return esp_lcd_panel_io_tx_param(s_panel_io, command, data, length);
}
esp_err_t box2_lcd_init(void)
{
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
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "configure backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "configure backlight PWM");
    const esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = BOX2_LCD_DC,
        .wr_gpio_num = BOX2_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            BOX2_LCD_D0, BOX2_LCD_D1, BOX2_LCD_D2, BOX2_LCD_D3,
            BOX2_LCD_D4, BOX2_LCD_D5, BOX2_LCD_D6, BOX2_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = BOX2_LCD_WIDTH * BOX2_LCD_HEIGHT * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    esp_lcd_i80_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG, "create LCD I80 bus");
    s_frame_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_frame_done, ESP_ERR_NO_MEM, TAG, "create LCD semaphore");
    xSemaphoreGive(s_frame_done);
    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = BOX2_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_transfer_done,
        .user_ctx = s_frame_done,
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
            .swap_color_bytes = 0,
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert panel color");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "set panel gap");
    static const uint8_t cf[] = {0x00,0x83,0x30};
    static const uint8_t ed[] = {0x64,0x03,0x12,0x81};
    static const uint8_t e8[] = {0x85,0x01,0x79};
    static const uint8_t cb[] = {0x39,0x2c,0x00,0x34,0x02};
    static const uint8_t e0[] = {0xd0,0x00,0x02,0x07,0x0a,0x28,0x32,0x44,0x42,0x06,0x0e,0x12,0x14,0x17};
    static const uint8_t e1[] = {0xd0,0x00,0x02,0x07,0x0a,0x28,0x31,0x54,0x47,0x0e,0x1c,0x17,0x1b,0x1e};
    static const struct { uint8_t cmd; uint8_t value; } middle_sequence[] = {
        {0xbb,0x20}, {0xc3,0x00}, {0xc4,0x20}, {0xc5,0x20}, {0xc6,0x10},
        {0xc7,0xb0}, {0x36,0x60}, {0x3a,0x55},
    };
    ESP_RETURN_ON_ERROR(lcd_command(0xcf, cf, sizeof(cf)), TAG, "LCD sequence CF");
    ESP_RETURN_ON_ERROR(lcd_command(0xed, ed, sizeof(ed)), TAG, "LCD sequence ED");
    ESP_RETURN_ON_ERROR(lcd_command(0xe8, e8, sizeof(e8)), TAG, "LCD sequence E8");
    ESP_RETURN_ON_ERROR(lcd_command(0xcb, cb, sizeof(cb)), TAG, "LCD sequence CB");
    const uint8_t ea[] = {0x00,0x00};
    const uint8_t b1[] = {0x00,0x1b};
    const uint8_t f7 = 0x20;
    const uint8_t f2 = 0x08;
    const uint8_t gamma = 0x01;
    const uint8_t b7 = 0x07;
    ESP_RETURN_ON_ERROR(lcd_command(0xf7, &f7, 1), TAG, "LCD sequence F7");
    ESP_RETURN_ON_ERROR(lcd_command(0xea, ea, sizeof(ea)), TAG, "LCD sequence EA");
    for (size_t i = 0; i < sizeof(middle_sequence) / sizeof(middle_sequence[0]); ++i) {
        ESP_RETURN_ON_ERROR(lcd_command(middle_sequence[i].cmd, &middle_sequence[i].value, 1),
                            TAG, "LCD one-byte sequence");
    }
    ESP_RETURN_ON_ERROR(lcd_command(0xb1, b1, sizeof(b1)), TAG, "LCD sequence B1");
    ESP_RETURN_ON_ERROR(lcd_command(0xf2, &f2, 1), TAG, "LCD sequence F2");
    ESP_RETURN_ON_ERROR(lcd_command(0x26, &gamma, 1), TAG, "LCD gamma select");
    ESP_RETURN_ON_ERROR(lcd_command(0xe0, e0, sizeof(e0)), TAG, "LCD gamma E0");
    ESP_RETURN_ON_ERROR(lcd_command(0xe1, e1, sizeof(e1)), TAG, "LCD gamma E1");
    ESP_RETURN_ON_ERROR(lcd_command(0xb7, &b7, 1), TAG, "LCD sequence B7");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "disable LCD XY swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "disable LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn display on");
    s_framebuffer = esp_lcd_i80_alloc_draw_buffer(
        s_panel_io, BOX2_LCD_WIDTH * BOX2_LCD_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_framebuffer, ESP_ERR_NO_MEM, TAG, "allocate LCD framebuffer in PSRAM");
    for (int duty = 32; duty <= 255; duty = duty == 32 ? 128 : 255) {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        vTaskDelay(pdMS_TO_TICKS(120));
        if (duty == 255) {
            break;
        }
    }
    return ESP_OK;
}
esp_err_t box2_lcd_show_color_test(void)
{
    s_dashboard_valid = false;
    ESP_RETURN_ON_ERROR(begin_frame(rgb565(0, 0, 0)), TAG, "wait for color frame");
    static const uint8_t colors[][3] = {
        {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255}, {255,0,255}, {255,255,0},
    };
    const int bar_width = BOX2_LCD_WIDTH / 6;
    for (int i = 0; i < 6; ++i) {
        fill_rect(i * bar_width, 0, bar_width, 145,
                  rgb565(colors[i][0], colors[i][1], colors[i][2]));
    }
    fill_rect(0, 145, BOX2_LCD_WIDTH, 175, rgb565(8, 14, 25));
    draw_text(24, 180, "BOX2 LCD TEST", 2, rgb565(255,255,255));
    draw_text(36, 220, "240 X 320", 2, rgb565(80,220,255));
    draw_text(12, 270, "RGB BARS AND BACKLIGHT", 1, rgb565(255,210,60));
    return present_frame();
}
esp_err_t box2_lcd_show_lines(const char *title, const char *const *lines,
                              size_t line_count, int meter_percent)
{
    ESP_RETURN_ON_FALSE(title && lines, ESP_ERR_INVALID_ARG, TAG, "invalid screen text");
    if (line_count > 17) {
        line_count = 17;
    }
    if (meter_percent < 0) meter_percent = 0;
    if (meter_percent > 100) meter_percent = 100;
    bool text_changed = !s_dashboard_valid || s_last_line_count != line_count ||
                        strcmp(s_last_title, title) != 0;
    for (size_t i = 0; !text_changed && i < line_count; ++i) {
        text_changed = strcmp(s_last_lines[i], lines[i]) != 0;
    }
    if (!text_changed) {
        if (meter_percent == s_last_meter) {
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(xSemaphoreTake(s_frame_done, pdMS_TO_TICKS(1000)) == pdTRUE,
                            ESP_ERR_TIMEOUT, TAG, "wait for meter frame");
        draw_meter(meter_percent);
        s_last_meter = meter_percent;
        return present_region(282, BOX2_LCD_HEIGHT);
    }
    ESP_RETURN_ON_ERROR(begin_frame(rgb565(5, 10, 20)), TAG, "wait for dashboard frame");
    fill_rect(0, 0, BOX2_LCD_WIDTH, 23, rgb565(10, 70, 125));
    draw_text(6, 7, title, 1, rgb565(255,255,255));
    for (size_t i = 0; i < line_count; ++i) {
        uint16_t color = rgb565(225, 235, 245);
        if (strstr(lines[i], " OK") || strstr(lines[i], "PASS")) {
            color = rgb565(70, 245, 130);
        } else if (strstr(lines[i], "FAIL")) {
            color = rgb565(255, 80, 80);
        }
        if (strstr(lines[i], "WAIT") || strstr(lines[i], "NO CARD")) {
            color = rgb565(255, 205, 70);
        }
        draw_text(6, 28 + (int)i * 15, lines[i], 1, color);
    }
    draw_meter(meter_percent);
    snprintf(s_last_title, sizeof(s_last_title), "%s", title);
    for (size_t i = 0; i < line_count; ++i) {
        snprintf(s_last_lines[i], sizeof(s_last_lines[i]), "%s", lines[i]);
    }
    s_last_line_count = line_count;
    s_last_meter = meter_percent;
    s_dashboard_valid = true;
    return present_frame();
}
