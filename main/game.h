#pragma once
#include <stdbool.h>
#include <stdint.h>
// One action only: bump the ball into the basket. Play starts immediately.
typedef struct {
    int score, baskets, high_score, volume;
    bool sensor_ok, audio_ok, music_ok, flying, scored, motion_ready, sample_ready;
    int64_t shot_at, feedback_at, last_motion, last_tick;
    float filtered[3], energy;
    float ball_x, ball_y;
} game_t;
#define BALL_START_X 126.0f
#define BALL_START_Y 144.0f
#define HOOP_X 190.0f
#define HOOP_Y 102.0f
#define SHOT_FLIGHT_MS 1100
#define SHOT_CYCLE_MS 1650
void game_init(game_t *g);
void game_restart(game_t *g,int64_t now);
bool game_motion(game_t *g,const float a[3],int64_t now);
bool game_bump(game_t *g,int64_t now);
void game_tick(game_t *g,int64_t now);
void game_render(const game_t *g,int64_t now,uint16_t *pixels);
