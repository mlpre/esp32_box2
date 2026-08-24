#include "hardware_test_screen.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "box2_lcd.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
static const char *TAG = "box2_test_screen";
static uint16_t *s_framebuffer;
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                                ((uint16_t)(g & 0xfc) << 3) | (b >> 3));
    return (uint16_t)((color << 8) | (color >> 8));
}

static esp_err_t ensure_framebuffer(void)
{
    if (!s_framebuffer)
    {
        s_framebuffer = heap_caps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_framebuffer ? ESP_OK : ESP_ERR_NO_MEM;
}

static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0)
    {
        width += x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        y = 0;
    }
    if (x + width > SCREEN_WIDTH)
        width = SCREEN_WIDTH - x;
    if (y + height > SCREEN_HEIGHT)
        height = SCREEN_HEIGHT - y;
    for (int row = y; row < y + height; ++row)
    {
        uint16_t *pixel = s_framebuffer + row * SCREEN_WIDTH + x;
        for (int col = 0; col < width; ++col)
            pixel[col] = color;
    }
}

static const uint8_t *glyph(char c)
{
    static const uint8_t digits[10][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e},
        {0x00, 0x42, 0x7f, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4b, 0x31},
        {0x18, 0x14, 0x12, 0x7f, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1e},
    };
    static const uint8_t letters[26][5] = {
        {0x7e, 0x11, 0x11, 0x11, 0x7e},
        {0x7f, 0x49, 0x49, 0x49, 0x36},
        {0x3e, 0x41, 0x41, 0x41, 0x22},
        {0x7f, 0x41, 0x41, 0x22, 0x1c},
        {0x7f, 0x49, 0x49, 0x49, 0x41},
        {0x7f, 0x09, 0x09, 0x09, 0x01},
        {0x3e, 0x41, 0x49, 0x49, 0x7a},
        {0x7f, 0x08, 0x08, 0x08, 0x7f},
        {0x00, 0x41, 0x7f, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3f, 0x01},
        {0x7f, 0x08, 0x14, 0x22, 0x41},
        {0x7f, 0x40, 0x40, 0x40, 0x40},
        {0x7f, 0x02, 0x0c, 0x02, 0x7f},
        {0x7f, 0x04, 0x08, 0x10, 0x7f},
        {0x3e, 0x41, 0x41, 0x41, 0x3e},
        {0x7f, 0x09, 0x09, 0x09, 0x06},
        {0x3e, 0x41, 0x51, 0x21, 0x5e},
        {0x7f, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7f, 0x01, 0x01},
        {0x3f, 0x40, 0x40, 0x40, 0x3f},
        {0x1f, 0x20, 0x40, 0x20, 0x1f},
        {0x3f, 0x40, 0x38, 0x40, 0x3f},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t space[5] = {0};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t percent[5] = {0x62, 0x64, 0x08, 0x13, 0x23};
    static const uint8_t equal[5] = {0x14, 0x14, 0x14, 0x14, 0x14};
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'];
    switch (c)
    {
    case '-':
        return dash;
    case '.':
        return dot;
    case ':':
        return colon;
    case '/':
        return slash;
    case '%':
        return percent;
    case '=':
        return equal;
    default:
        return space;
    }
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *bitmap = glyph(c);
    for (int col = 0; col < 5; ++col)
    {
        for (int row = 0; row < 7; ++row)
        {
            if (bitmap[col] & (1U << row))
            {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    while (*text && x + 5 * scale < SCREEN_WIDTH)
    {
        draw_char(x, y, *text++, scale, color);
        x += 6 * scale;
    }
}

static void draw_meter(int meter_percent)
{
    fill_rect(0, 282, SCREEN_WIDTH, 38, rgb565(5, 10, 20));
    draw_text(6, 284, "MIC", 1, rgb565(150, 170, 190));
    fill_rect(30, 283, 204, 11, rgb565(25, 35, 50));
    fill_rect(32, 285, meter_percent * 200 / 100, 7,
              meter_percent > 85 ? rgb565(255, 70, 50) : rgb565(40, 210, 100));
    draw_text(6, 301, "LIVE AUDIO INPUT LEVEL", 1, rgb565(100, 190, 230));
}

esp_err_t hardware_test_screen_show_color_bars(void)
{
    ESP_RETURN_ON_ERROR(ensure_framebuffer(), TAG, "allocate test framebuffer");
    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, rgb565(0, 0, 0));
    static const uint8_t colors[][3] = {
        {255, 0, 0},
        {0, 255, 0},
        {0, 0, 255},
        {0, 255, 255},
        {255, 0, 255},
        {255, 255, 0},
    };
    for (int i = 0; i < 6; ++i)
    {
        fill_rect(i * 40, 0, 40, 145, rgb565(colors[i][0], colors[i][1], colors[i][2]));
    }
    fill_rect(0, 145, SCREEN_WIDTH, 175, rgb565(8, 14, 25));
    draw_text(24, 180, "BOX2 LCD TEST", 2, rgb565(255, 255, 255));
    draw_text(36, 220, "240 X 320", 2, rgb565(80, 220, 255));
    draw_text(12, 270, "RGB BARS AND BACKLIGHT", 1, rgb565(255, 210, 60));
    return box2_lcd_draw_bitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, s_framebuffer);
}

esp_err_t hardware_test_screen_show_lines(const char *title,
                                          const char *const *lines,
                                          size_t line_count,
                                          int meter_percent)
{
    ESP_RETURN_ON_FALSE(title && lines, ESP_ERR_INVALID_ARG, TAG, "invalid screen text");
    ESP_RETURN_ON_ERROR(ensure_framebuffer(), TAG, "allocate test framebuffer");
    if (line_count > 17)
        line_count = 17;
    if (meter_percent < 0)
        meter_percent = 0;
    if (meter_percent > 100)
        meter_percent = 100;
    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, rgb565(5, 10, 20));
    fill_rect(0, 0, SCREEN_WIDTH, 23, rgb565(10, 70, 125));
    draw_text(6, 7, title, 1, rgb565(255, 255, 255));
    for (size_t i = 0; i < line_count; ++i)
    {
        uint16_t color = rgb565(225, 235, 245);
        if (strstr(lines[i], " OK") || strstr(lines[i], "PASS"))
            color = rgb565(70, 245, 130);
        else if (strstr(lines[i], "FAIL"))
            color = rgb565(255, 80, 80);
        else if (strstr(lines[i], "WAIT") || strstr(lines[i], "NO CARD"))
            color = rgb565(255, 205, 70);
        draw_text(6, 28 + (int)i * 15, lines[i], 1, color);
    }
    draw_meter(meter_percent);
    return box2_lcd_draw_bitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, s_framebuffer);
}
