#include "radio_screen.h"
#include "radio_font.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "box2_lcd.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUFFER_LINES 40

static const char *TAG = "radio_screen";
static lv_display_t *s_display;
static lv_obj_t *s_top_volume;
static lv_obj_t *s_top_time;
static lv_obj_t *s_top_battery;
static lv_obj_t *s_station_name;

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0)
    {
        lv_label_set_text(label, text);
    }
}

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lcd_flush(lv_display_t *display, const lv_area_t *area,
                      uint8_t *pixel_map)
{
    uint32_t pixel_count = (uint32_t)(area->x2 - area->x1 + 1) *
                           (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(pixel_map, pixel_count);
    esp_err_t result = box2_lcd_draw_bitmap(area->x1, area->y1,
                                            area->x2 + 1, area->y2 + 1,
                                            (const uint16_t *)pixel_map);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(result));
    }
    lv_display_flush_ready(display);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int width,
                            int height)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int width,
                            int height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    return panel;
}

static void make_action_icon(lv_obj_t *screen, int x, const char *symbol)
{
    lv_obj_t *icon = lv_label_create(screen);
    lv_obj_set_pos(icon, x, 286);
    lv_obj_set_size(icon, 40, 24);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(icon, symbol);
}

static esp_err_t initialize_screen(void)
{
    if (s_display)
    {
        return ESP_OK;
    }

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);
    size_t buffer_bytes = SCREEN_WIDTH * DRAW_BUFFER_LINES * sizeof(lv_color16_t);
    void *draw_buffer = heap_caps_malloc(buffer_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(draw_buffer, ESP_ERR_NO_MEM, TAG,
                        "allocate LVGL draw buffer");

    s_display = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_NO_MEM, TAG, "create LVGL display");
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, draw_buffer, NULL, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, lcd_flush);

    lv_theme_t *theme = lv_theme_default_init(
        s_display,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        true,
        &radio_font_16);
    lv_display_set_theme(s_display, theme);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top_bar = make_panel(screen, 8, 6, 224, 34);
    s_top_volume = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_volume, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_align(s_top_volume, LV_ALIGN_LEFT_MID, 0, 0);
    s_top_time = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_time, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_align(s_top_time, LV_ALIGN_CENTER, 0, 0);
    s_top_battery = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_battery, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_align(s_top_battery, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *hero = make_panel(screen, 8, 48, 224, 222);
    s_station_name = make_label(hero, 0, 0, 190, 72);
    lv_obj_align(s_station_name, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_station_name, &radio_font_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_station_name, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_station_name, LV_LABEL_LONG_WRAP);

    make_action_icon(screen, 10, LV_SYMBOL_PREV);
    make_action_icon(screen, 70, LV_SYMBOL_MINUS);
    make_action_icon(screen, 130, LV_SYMBOL_PLUS);
    make_action_icon(screen, 190, LV_SYMBOL_NEXT);
    return ESP_OK;
}

esp_err_t radio_screen_show(const radio_screen_data_t *data)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "screen data is null");
    ESP_RETURN_ON_ERROR(initialize_screen(), TAG, "initialize premium Chinese UI");

    char line[160];
    set_label_text_if_changed(s_station_name,
                              radio_stream_station_name(data->radio.station_index));

    const char *volume_icon = data->radio.volume_percent == 0 ? LV_SYMBOL_MUTE :
                              data->radio.volume_percent < 60 ? LV_SYMBOL_VOLUME_MID :
                              LV_SYMBOL_VOLUME_MAX;
    snprintf(line, sizeof(line), "%s %d%%", volume_icon,
             data->radio.volume_percent);
    set_label_text_if_changed(s_top_volume, line);

    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    if (local_time.tm_year >= 124)
    {
        snprintf(line, sizeof(line), "%02d:%02d",
                 local_time.tm_hour, local_time.tm_min);
    }
    else
    {
        snprintf(line, sizeof(line), "--:--");
    }
    set_label_text_if_changed(s_top_time, line);

    const char *battery_icon = data->battery_percent >= 90 ? LV_SYMBOL_BATTERY_FULL :
                               data->battery_percent >= 65 ? LV_SYMBOL_BATTERY_3 :
                               data->battery_percent >= 40 ? LV_SYMBOL_BATTERY_2 :
                               data->battery_percent >= 15 ? LV_SYMBOL_BATTERY_1 :
                               LV_SYMBOL_BATTERY_EMPTY;
    snprintf(line, sizeof(line), "%s %d%%", battery_icon,
             data->battery_percent);
    set_label_text_if_changed(s_top_battery, line);

    lv_timer_handler();
    return ESP_OK;
}
