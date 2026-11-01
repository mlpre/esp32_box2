#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "box2.h"
#include "device_controls.h"
#include "device_screen.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_microphone.h"
#include "usb_mode.h"

static const char *TAG = "box2";

typedef struct
{
    bool raw;
    bool pressed;
    uint32_t changed_at;
} button_filter_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool filtered_button(button_filter_t *button, bool raw, uint32_t now)
{
    if (raw != button->raw)
    {
        button->raw = raw;
        button->changed_at = now;
    }
    if (button->pressed != raw && (uint32_t)(now - button->changed_at) >= 30)
    {
        button->pressed = raw;
    }
    return button->pressed;
}

static int discard_usb_log(const char *format, va_list arguments)
{
    (void)format;
    (void)arguments;
    return 0;
}

static void enter_power_off(bool microphone, bool lcd_ok)
{
    usb_mode_set_commands_enabled(false);
    if (lcd_ok)
    {
        (void)device_screen_show_power_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    // Keep console writes away from the shared PHY while USB is detached.
    esp_log_set_vprintf(discard_usb_log);
    if (microphone)
    {
        usb_microphone_prepare_power_off();
    }
    else
    {
        usb_mode_disconnect_serial_phy();
    }
    if (lcd_ok)
    {
        (void)box2_lcd_set_backlight(0);
    }
    // Battery power normally disappears here. USB power can keep the CPU
    // alive, so the caller also enters a dark, USB-disconnected standby loop.
    // That fallback is functional standby, not an ESP deep-sleep mode.
    (void)box2_board_power_off();
}

static void run_device(bool microphone, esp_err_t initial_error)
{
    if (microphone)
    {
        esp_log_set_vprintf(discard_usb_log);
    }
    const bool board_ok = box2_board_init() == ESP_OK;
    if (board_ok)
    {
        (void)box2_board_set_speaker_enabled(false);
    }
    if (microphone)
    {
        usb_mode_disconnect_serial_phy();
    }
    const bool lcd_ok = box2_lcd_init() == ESP_OK;
    // LCD initialization leaves the backlight off. Light it after a valid
    // first frame rather than exposing panel RAM during codec/USB startup.
    bool screen_lit = false;
    esp_err_t audio_error = ESP_ERR_INVALID_STATE;
    esp_err_t usb_error = ESP_ERR_INVALID_STATE;
    int gain_db = 30;
    if (microphone && board_ok)
    {
        audio_error = box2_audio_init_with_sample_rate(box2_board_i2c_bus(), 48000);
        if (audio_error == ESP_OK)
        {
            audio_error = box2_audio_set_input_gain((float)gain_db);
        }
        usb_error = audio_error == ESP_OK ? usb_microphone_start() : audio_error;
    }
    else if (!microphone)
    {
        usb_error = usb_mode_start_serial_commands();
        ESP_LOGI(TAG, "Serial mode ready. Hold Q to switch, hold M to power off.");
    }

    box2_board_state_t board = {0};
    device_controls_t controls;
    bool controls_ready = false;
    button_filter_t left = {0};
    button_filter_t right = {0};
    bool previous_left = false;
    bool previous_right = false;
    bool local_muted = false;
    bool battery_valid = false;
    bool power_pending = false;
    bool power_releasing = false;
    bool standby = false;
    uint32_t power_released_at = 0;
    esp_err_t action_error = initial_error;
    uint32_t last_battery = now_ms() - 5000;
    uint32_t last_screen = now_ms() - 200;
    uint32_t last_capture = now_ms();
    uint32_t previous_bytes = 0;
    bool capture_seen = false;
    int level_percent = 0;

    while (true)
    {
        uint32_t now = now_ms();
        const bool sample_battery = !standby && !power_pending &&
                                    (uint32_t)(now - last_battery) >= 5000;
        // Key polling remains active after power-off only if external power
        // keeps the MCU/expander alive. Never toggle CHG_CTRL to sample then.
        const bool inputs_ok = board_ok &&
                               box2_board_read_state(&board, sample_battery) == ESP_OK;
        now = now_ms();
        if (inputs_ok)
        {
            if (sample_battery)
            {
                last_battery = now;
                battery_valid = true;
            }
            uint32_t events = DEVICE_CONTROL_NONE;
            if (!controls_ready)
            {
                device_controls_init(&controls, board.middle_pressed, board.q_pressed, now);
                controls_ready = true;
            }
            else
            {
                events = device_controls_update(&controls, board.middle_pressed,
                                                board.q_pressed, now);
            }

            if (standby)
            {
                if (events & DEVICE_CONTROL_M_SHORT_RELEASE)
                {
                    // M is now released, so boot cannot consume this press as
                    // mute/shutdown. Restart reinitializes the saved USB mode.
                    if (box2_board_power_on() == ESP_OK)
                    {
                        esp_restart();
                    }
                }
            }
            else
            {
                if (events & DEVICE_CONTROL_M_LONG_PRESS)
                {
                    power_pending = true;
                    power_releasing = false;
                    action_error = ESP_OK;
                    usb_mode_set_commands_enabled(false);
                }
                if (power_pending)
                {
                    // M also drives the hardware power-on path. Release it
                    // before clearing SYS_POW, or it can keep power asserted.
                    if (board.middle_pressed || controls.middle.pressed)
                    {
                        power_releasing = false;
                    }
                    else
                    {
                        if (!power_releasing)
                        {
                            power_releasing = true;
                            power_released_at = now;
                        }
                        if ((uint32_t)(now - power_released_at) >= 100)
                        {
                            enter_power_off(microphone, lcd_ok);
                            standby = true;
                        }
                    }
                }
                else
                {
                    if (events & DEVICE_CONTROL_Q_LONG_PRESS)
                    {
                        action_error = ESP_OK;
                    }
                    if (events & DEVICE_CONTROL_Q_LONG_RELEASED)
                    {
                        const usb_mode_t target = microphone ? USB_MODE_SERIAL : USB_MODE_MICROPHONE;
                        action_error = usb_mode_switch(target);
                        if (action_error != ESP_OK)
                        {
                            ESP_LOGE(TAG, "USB mode unchanged: %s", esp_err_to_name(action_error));
                        }
                    }
                    if (microphone && (events & DEVICE_CONTROL_M_SHORT_RELEASE))
                    {
                        local_muted = !local_muted;
                        usb_microphone_set_muted(local_muted);
                    }
                    const bool l = filtered_button(&left, board.left_pressed, now);
                    const bool r = filtered_button(&right, board.right_pressed, now);
                    int requested_gain = gain_db;
                    if (microphone && l && !previous_left && requested_gain >= 3)
                    {
                        requested_gain -= 3;
                    }
                    if (microphone && r && !previous_right && requested_gain <= 39)
                    {
                        requested_gain += 3;
                    }
                    if (requested_gain != gain_db && audio_error == ESP_OK)
                    {
                        const esp_err_t gain_error = box2_audio_set_input_gain((float)requested_gain);
                        if (gain_error == ESP_OK)
                        {
                            gain_db = requested_gain;
                        }
                        else
                        {
                            action_error = gain_error;
                        }
                    }
                    previous_left = l;
                    previous_right = r;
                }
            }
        }

        if (!standby && lcd_ok && (uint32_t)(now - last_screen) >= 200)
        {
            usb_microphone_stats_t stats = {0};
            if (microphone)
            {
                usb_microphone_get_stats(&stats);
                if (stats.bytes_sent != previous_bytes)
                {
                    last_capture = now;
                    capture_seen = true;
                }
                previous_bytes = stats.bytes_sent;
            }
            const bool connected = microphone ? usb_error == ESP_OK && tud_mounted()
                                              : usb_serial_jtag_is_connected();
            const bool recording = microphone && connected && capture_seen &&
                                   (uint32_t)(now - last_capture) < 500;
            const bool muted = microphone && usb_microphone_is_muted();
            const int target_level = recording && !muted ? stats.peak * 100 / 32768 : 0;
            level_percent = target_level >= level_percent ? target_level :
                            (3 * level_percent + target_level) / 4;
            if (!recording || muted)
            {
                level_percent = 0;
            }
            device_screen_state_t screen = {
                .mode = microphone ? DEVICE_SCREEN_MODE_MICROPHONE : DEVICE_SCREEN_MODE_SERIAL,
                .usb_connected = connected,
                .recording = recording,
                .audio_ready = audio_error == ESP_OK,
                .muted = muted,
                .charging = board.charging,
                .battery_percent = battery_valid ? board.battery_percent : -1,
                .gain_db = gain_db,
                .level_percent = level_percent,
                .action = action_error == ESP_OK && board_ok && usb_error == ESP_OK
                              ? DEVICE_SCREEN_ACTION_NONE : DEVICE_SCREEN_ACTION_ERROR,
            };
            if (controls_ready)
            {
                if (power_pending || device_controls_hold_percent(&controls.middle, now) > 0)
                {
                    screen.action = DEVICE_SCREEN_ACTION_POWER_OFF;
                    screen.action_progress_percent = power_pending ? 100 :
                        device_controls_hold_percent(&controls.middle, now);
                }
                else if (controls.q_pending || device_controls_hold_percent(&controls.q, now) > 0)
                {
                    screen.action = DEVICE_SCREEN_ACTION_SWITCH_MODE;
                    screen.action_progress_percent = controls.q_pending ? 100 :
                        device_controls_hold_percent(&controls.q, now);
                }
            }
            if (device_screen_show(&screen) == ESP_OK && !screen_lit)
            {
                (void)box2_lcd_set_backlight(85);
                screen_lit = true;
            }
            last_screen = now;
        }
        vTaskDelay(pdMS_TO_TICKS(standby ? 40 : 20));
    }
}

void app_main(void)
{
    // M momentarily powers the board during a battery boot. Latch SYS_POW
    // before NVS work or the 200 ms USB detach so a short press can start it.
    // Restore the retained PHY first, before the board driver emits any logs.
    usb_mode_restore_serial_phy();
    (void)box2_board_init();
    const esp_err_t mode_error = usb_mode_init();
    if (mode_error != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS mode unavailable; using serial: %s", esp_err_to_name(mode_error));
    }
    bool microphone = usb_mode_current() == USB_MODE_MICROPHONE;
    if (microphone)
    {
        const esp_reset_reason_t reason = esp_reset_reason();
        if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
            reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
        {
            ESP_LOGW(TAG, "Microphone crashed; restoring serial mode");
            (void)usb_mode_switch(USB_MODE_SERIAL);
            // If saving failed, still provide serial recovery for this boot.
            microphone = false;
        }
    }
    run_device(microphone, mode_error);
}
