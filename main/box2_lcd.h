#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BOX2_GAME_TRAFFIC_MAX 5

typedef enum {
    BOX2_GAME_TITLE = 0,
    BOX2_GAME_COUNTDOWN,
    BOX2_GAME_RUNNING,
    BOX2_GAME_PAUSED,
    BOX2_GAME_OVER,
} box2_game_screen_t;

typedef struct {
    float lane_x;
    float depth;
    uint8_t color;
    bool active;
} box2_game_traffic_t;

typedef struct {
    box2_game_screen_t screen;
    float player_x;
    float road_phase;
    float curve;
    float speed_kmh;
    int score;
    int distance_m;
    int best_score;
    int health;
    int volume;
    int countdown;
    int tilt_mg;
    int steering_axis;
    int steering_sign;
    bool steering_ready;
    bool muted;
    box2_game_traffic_t traffic[BOX2_GAME_TRAFFIC_MAX];
} box2_game_frame_t;

esp_err_t box2_lcd_init(void);
esp_err_t box2_lcd_show_color_test(void);
esp_err_t box2_lcd_show_lines(const char *title, const char *const *lines,
                              size_t line_count, int meter_percent);
esp_err_t box2_lcd_render_racing(const box2_game_frame_t *game);
