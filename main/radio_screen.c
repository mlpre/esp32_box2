#include "radio_screen.h"
#include "radio_font.h"
#include "weather_icons.h"

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
static lv_obj_t *s_weather_icon;
static lv_obj_t *s_weather_summary;
static lv_obj_t *s_weather_temperature;
static lv_obj_t *s_date;
static lv_obj_t *s_station_name;
static const lv_image_dsc_t *s_last_weather_image;

static const char *weather_text(weather_icon_t icon)
{
    switch (icon)
    {
    case WEATHER_ICON_CLEAR:
        return "晴";
    case WEATHER_ICON_PARTLY_CLOUDY:
        return "少云";
    case WEATHER_ICON_CLOUDY:
        return "多云";
    case WEATHER_ICON_FOG:
        return "雾";
    case WEATHER_ICON_RAIN:
        return "雨";
    case WEATHER_ICON_SNOW:
        return "雪";
    case WEATHER_ICON_THUNDER:
        return "雷雨";
    default:
        return "";
    }
}

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
    lv_obj_remove_style_all(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    return panel;
}

static void make_action_icon(lv_obj_t *screen, int x, const char *symbol)
{
    lv_obj_t *icon = lv_label_create(screen);
    lv_obj_set_pos(icon, x, 282);
    lv_obj_set_size(icon, 40, 30);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xC7D2DE), LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x08111C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *top_bar = make_panel(screen, 8, 3, 224, 34);
    s_top_volume = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_volume, &lv_font_montserrat_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_top_volume, lv_color_hex(0xAAB7C4),
                                LV_PART_MAIN);
    lv_obj_align(s_top_volume, LV_ALIGN_LEFT_MID, 0, 0);
    s_top_time = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_time, &lv_font_montserrat_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_top_time, lv_color_hex(0xF4F7FA),
                                LV_PART_MAIN);
    lv_obj_align(s_top_time, LV_ALIGN_CENTER, 0, 0);
    s_top_battery = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_top_battery, &lv_font_montserrat_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_top_battery, lv_color_hex(0xAAB7C4),
                                LV_PART_MAIN);
    lv_obj_align(s_top_battery, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *hero = make_panel(screen, 8, 42, 224, 234);
    lv_obj_t *weather_card = make_panel(hero, 0, 0, 212, 140);
    lv_obj_align(weather_card, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(weather_card, lv_color_hex(0x162433), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(weather_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(weather_card, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(weather_card, lv_color_hex(0x000000),
                                  LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(weather_card, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(weather_card, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(weather_card, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(weather_card, 0, LV_PART_MAIN);

    s_weather_icon = lv_image_create(weather_card);
    lv_obj_set_pos(s_weather_icon, 2, 22);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);

    s_weather_summary = make_label(weather_card, 100, 23, 102, 26);
    lv_obj_set_style_text_font(s_weather_summary, &radio_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_weather_summary, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(s_weather_summary, lv_color_hex(0xAFC0D0),
                                LV_PART_MAIN);

    s_weather_temperature = make_label(weather_card, 104, 52, 94, 34);
    lv_obj_set_style_text_font(s_weather_temperature, &radio_font_24,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_weather_temperature, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(s_weather_temperature, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    s_date = make_label(weather_card, 10, 108, 192, 26);
    lv_obj_set_style_text_font(s_date, &radio_font_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date, lv_color_hex(0x8FA3B6), LV_PART_MAIN);

    lv_obj_t *station_accent = lv_obj_create(hero);
    lv_obj_remove_style_all(station_accent);
    lv_obj_set_size(station_accent, 28, 3);
    lv_obj_align(station_accent, LV_ALIGN_TOP_MID, 0, 159);
    lv_obj_set_style_bg_color(station_accent, lv_color_hex(0x53BDEB),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(station_accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(station_accent, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    s_station_name = make_label(hero, 0, 0, 204, 64);
    lv_obj_align(s_station_name, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_text_font(s_station_name, &radio_font_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_station_name, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(s_station_name, lv_color_hex(0xF2F6FA),
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
    char station_name[128];
    if (data->radio.state == RADIO_STATE_LOADING_DIRECTORY)
    {
        strlcpy(station_name, "正在更新电台", sizeof(station_name));
    }
    else
    {
        radio_stream_get_station_name(data->radio.station_index, station_name,
                                      sizeof(station_name));
    }
    set_label_text_if_changed(s_station_name, station_name);

    if (data->weather.valid)
    {
        const lv_image_dsc_t *image = weather_icon_asset(
            data->weather.icon, data->weather.is_day);
        if (image)
        {
            if (image != s_last_weather_image)
            {
                lv_image_set_src(s_weather_icon, image);
                s_last_weather_image = image;
            }
            lv_obj_remove_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(line, sizeof(line), "%s·%s", data->weather.city,
                 weather_text(data->weather.icon));
        set_label_text_if_changed(s_weather_summary, line);
        snprintf(line, sizeof(line), "%d°C", data->weather.temperature_c);
        set_label_text_if_changed(s_weather_temperature, line);
    }
    else
    {
        lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_weather_summary, "");
        set_label_text_if_changed(s_weather_temperature, "");
    }

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
        static const char *weekdays[] = {
            "星期日", "星期一", "星期二", "星期三",
            "星期四", "星期五", "星期六",
        };
        snprintf(line, sizeof(line), "%d月%d日  %s", local_time.tm_mon + 1,
                 local_time.tm_mday, weekdays[local_time.tm_wday]);
        set_label_text_if_changed(s_date, line);
        snprintf(line, sizeof(line), "%02d:%02d",
                 local_time.tm_hour, local_time.tm_min);
    }
    else
    {
        set_label_text_if_changed(s_date, "1月1日  星期一");
        snprintf(line, sizeof(line), "00:00");
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
