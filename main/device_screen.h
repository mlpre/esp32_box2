#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum
{
    DEVICE_SCREEN_MODE_SERIAL = 0,
    DEVICE_SCREEN_MODE_MICROPHONE,
} device_screen_mode_t;

typedef enum
{
    DEVICE_SCREEN_ACTION_NONE = 0,
    DEVICE_SCREEN_ACTION_SWITCH_MODE,
    DEVICE_SCREEN_ACTION_POWER_OFF,
    DEVICE_SCREEN_ACTION_ERROR,
} device_screen_action_t;

typedef struct
{
    device_screen_mode_t mode;
    bool usb_connected;
    bool audio_ready;
    bool recording;      // Host has an open capture stream.
    bool listening;       // Push-to-talk is holding the microphone open.
    bool charging;
    int battery_percent; // -1 means unavailable.
    int gain_db;
    int level_percent; // 0..100; MIC idle/disconnected displays zero.
    device_screen_action_t action;
    int action_progress_percent; // 100 means release the key to confirm.
} device_screen_state_t;

// Call from one UI task. Allocates its framebuffer lazily and sends RGB565
// frames through the existing LCD driver. Does not change LCD/backlight state.
esp_err_t device_screen_show(const device_screen_state_t *state);
esp_err_t device_screen_show_power_off(void);
