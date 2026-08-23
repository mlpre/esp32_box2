#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "box2_audio.h"
#include "box2_board.h"
#include "box2_lcd.h"
#include "box2_motion.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "neon_rush";

typedef struct {
    box2_game_frame_t view;
    float distance_exact;
    float lateral_velocity;
    float curve_target;
    float curve_timer;
    int bonus_score;
    int invulnerable_ms;
    int center_x_mg;
    int center_y_mg;
    uint32_t random;
    TickType_t countdown_started;
} race_game_t;

static uint32_t race_random(race_game_t *game)
{
    uint32_t x = game->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->random = x;
    return x;
}

static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void reset_traffic(race_game_t *game)
{
    static const float lanes[] = {-0.72f, 0.0f, 0.72f};
    for (int i = 0; i < BOX2_GAME_TRAFFIC_MAX; ++i) {
        game->view.traffic[i].active = true;
        game->view.traffic[i].depth = 0.08f + i * 0.18f;
        game->view.traffic[i].lane_x = lanes[race_random(game) % 3];
        game->view.traffic[i].color = (uint8_t)(1 + race_random(game) % 5);
    }
}

static void start_race(race_game_t *game, TickType_t now)
{
    int best = game->view.best_score;
    int volume = game->view.volume;
    int tilt = game->view.tilt_mg;
    memset(&game->view, 0, sizeof(game->view));
    game->view.screen = BOX2_GAME_COUNTDOWN;
    game->view.countdown = 3;
    game->view.health = 100;
    game->view.volume = volume;
    game->view.best_score = best;
    game->view.tilt_mg = tilt;
    game->distance_exact = 0.0f;
    game->lateral_velocity = 0.0f;
    game->curve_target = 0.0f;
    game->curve_timer = 2.0f;
    game->bonus_score = 0;
    game->invulnerable_ms = 0;
    game->countdown_started = now;
    reset_traffic(game);
    box2_audio_set_engine(0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_START));
}

static void set_volume(race_game_t *game, int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == game->view.volume) return;
    game->view.volume = volume;
    game->view.muted = volume == 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_set_volume(volume));
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_MENU));
}

static void update_traffic(race_game_t *game, float dt)
{
    static const float lanes[] = {-0.72f, 0.0f, 0.72f};
    float advance = dt * (0.085f + game->view.speed_kmh / 850.0f);
    for (int i = 0; i < BOX2_GAME_TRAFFIC_MAX; ++i) {
        box2_game_traffic_t *car = &game->view.traffic[i];
        if (!car->active) continue;
        car->depth += advance;
        if (car->depth > 1.08f) {
            car->depth = 0.04f + (race_random(game) % 8) * 0.01f;
            car->lane_x = lanes[race_random(game) % 3];
            car->color = (uint8_t)(1 + race_random(game) % 5);
            game->bonus_score += 100;
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_PASS));
            continue;
        }
        if (game->invulnerable_ms <= 0 && car->depth > 0.79f && car->depth < 1.01f &&
            fabsf(game->view.player_x - car->lane_x) < 0.34f) {
            game->view.health -= 34;
            if (game->view.health < 0) game->view.health = 0;
            game->view.speed_kmh *= 0.38f;
            game->lateral_velocity += game->view.player_x > car->lane_x ? 0.9f : -0.9f;
            game->invulnerable_ms = 1100;
            car->depth = 1.09f;
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_CRASH));
        }
    }
}

static void update_running(race_game_t *game, const box2_motion_state_t *motion,
                           float dt)
{
    int steer_mg = motion->x_mg - game->center_x_mg;
    int throttle_mg = motion->y_mg - game->center_y_mg;
    game->view.tilt_mg = steer_mg;
    float steering = clampf(steer_mg / 520.0f, -1.0f, 1.0f);
    float target_speed = 142.0f - throttle_mg * 0.11f;
    target_speed = clampf(target_speed, 72.0f, 224.0f);
    if (fabsf(game->view.player_x) > 0.84f) target_speed *= 0.48f;
    game->view.speed_kmh += (target_speed - game->view.speed_kmh) * dt * 1.7f;
    game->lateral_velocity += steering * dt * 4.4f;
    game->lateral_velocity *= 1.0f - clampf(dt * 3.6f, 0.0f, 0.9f);
    game->view.player_x += game->lateral_velocity * dt;
    if (game->view.player_x < -1.05f) {
        game->view.player_x = -1.05f;
        game->lateral_velocity = 0.1f;
    }
    if (game->view.player_x > 1.05f) {
        game->view.player_x = 1.05f;
        game->lateral_velocity = -0.1f;
    }
    game->curve_timer -= dt;
    if (game->curve_timer <= 0.0f) {
        game->curve_target = ((int)(race_random(game) % 161) - 80) / 100.0f;
        game->curve_timer = 3.2f + (race_random(game) % 35) / 10.0f;
    }
    game->view.curve += (game->curve_target - game->view.curve) * dt * 0.34f;
    game->view.road_phase += game->view.speed_kmh * dt / 96.0f;
    while (game->view.road_phase >= 1.0f) game->view.road_phase -= 1.0f;
    game->distance_exact += game->view.speed_kmh * dt / 3.6f;
    game->view.distance_m = (int)game->distance_exact;
    game->view.score = game->bonus_score + game->view.distance_m * 2;
    if (game->invulnerable_ms > 0) game->invulnerable_ms -= (int)(dt * 1000.0f);
    update_traffic(game, dt);
    box2_audio_set_engine((int)(game->view.speed_kmh * 100.0f / 224.0f));
    if (game->view.health <= 0) {
        game->view.screen = BOX2_GAME_OVER;
        if (game->view.score > game->view.best_score) game->view.best_score = game->view.score;
        box2_audio_set_engine(0);
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_GAME_OVER));
    }
}

static void update_countdown(race_game_t *game, TickType_t now)
{
    uint32_t elapsed_ms = (uint32_t)((now - game->countdown_started) * portTICK_PERIOD_MS);
    game->view.countdown = 3 - (int)(elapsed_ms / 1000);
    game->view.road_phase += 0.006f;
    if (game->view.road_phase >= 1.0f) game->view.road_phase -= 1.0f;
    if (elapsed_ms >= 3000) {
        game->view.screen = BOX2_GAME_RUNNING;
        game->view.countdown = 0;
        game->view.speed_kmh = 78.0f;
    }
}

static void calibrate_motion(race_game_t *game, box2_motion_state_t *motion)
{
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int samples = 0;
    for (int i = 0; i < 48; ++i) {
        if (box2_motion_read(motion) == ESP_OK) {
            sum_x += motion->x_mg;
            sum_y += motion->y_mg;
            ++samples;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (samples > 0) {
        game->center_x_mg = (int)(sum_x / samples);
        game->center_y_mg = (int)(sum_y / samples);
    }
    ESP_LOGI(TAG, "tilt center calibrated: x=%d mg y=%d mg",
             game->center_x_mg, game->center_y_mg);
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEON RUSH starting");
    ESP_ERROR_CHECK(box2_board_init());
    ESP_ERROR_CHECK(box2_lcd_init());
    ESP_ERROR_CHECK(box2_motion_init(box2_board_i2c_bus()));
    bool audio_ok = box2_audio_init(box2_board_i2c_bus()) == ESP_OK;
    if (!audio_ok) ESP_LOGW(TAG, "audio unavailable; game will continue silently");

    race_game_t game = {
        .view = {
            .screen = BOX2_GAME_TITLE,
            .health = 100,
            .volume = 50,
        },
        .random = 0x83f19a27,
    };
    reset_traffic(&game);
    if (audio_ok) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_set_volume(50));
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_MENU));
    }
    box2_motion_state_t motion = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_lcd_render_racing(&game.view));
    calibrate_motion(&game, &motion);

    box2_board_state_t keys = {0};
    box2_board_state_t previous = {0};
    TickType_t last_tick = xTaskGetTickCount();
    TickType_t last_render = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        float dt = (float)(now - last_tick) * portTICK_PERIOD_MS / 1000.0f;
        last_tick = now;
        if (dt > 0.08f) dt = 0.08f;
        if (box2_board_read_state(&keys, false) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (box2_motion_read(&motion) != ESP_OK) motion.detected = false;
        bool left_edge = keys.left_pressed && !previous.left_pressed;
        bool right_edge = keys.right_pressed && !previous.right_pressed;
        bool middle_edge = keys.middle_pressed && !previous.middle_pressed;
        bool q_edge = keys.q_pressed && !previous.q_pressed;
        previous = keys;

        if (left_edge) set_volume(&game, game.view.volume - 10);
        if (right_edge) set_volume(&game, game.view.volume + 10);
        if (q_edge) start_race(&game, now);
        if (middle_edge) {
            if (game.view.screen == BOX2_GAME_TITLE || game.view.screen == BOX2_GAME_OVER) {
                start_race(&game, now);
            } else if (game.view.screen == BOX2_GAME_RUNNING) {
                game.view.screen = BOX2_GAME_PAUSED;
                box2_audio_set_engine(0);
                ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_MENU));
            } else if (game.view.screen == BOX2_GAME_PAUSED) {
                game.view.screen = BOX2_GAME_RUNNING;
                ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_effect(BOX2_SFX_START));
            }
        }

        if (game.view.screen == BOX2_GAME_COUNTDOWN) {
            update_countdown(&game, now);
        } else if (game.view.screen == BOX2_GAME_RUNNING && motion.detected) {
            update_running(&game, &motion, dt);
        } else if (game.view.screen == BOX2_GAME_TITLE) {
            game.view.road_phase += dt * 0.12f;
            if (game.view.road_phase >= 1.0f) game.view.road_phase -= 1.0f;
        }

        if (now - last_render >= pdMS_TO_TICKS(50)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_lcd_render_racing(&game.view));
            last_render = now;
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}
