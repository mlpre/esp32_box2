#include "usb_hid.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tusb.h"

enum
{
    HID_COMBO_CAPACITY = 24,
    HID_MOD_LCTRL = 0x01,
    HID_MOD_LSHIFT = 0x02,
    HID_MOD_LALT = 0x04,
    HID_MOD_LGUI = 0x08,
    HID_RELEASE_GAP_MS = 20,
    HID_READY_RETRY = 3,
};

#define HID_NVS_NAMESPACE "box2_hid"
#define HID_NVS_COMBO "combo"
#define HID_NVS_MODE "mode"
// The key pair sent when nothing has been configured yet.
#define HID_DEFAULT_COMBO "ctrl+d"

static const char *TAG = "usb_hid";

static uint8_t s_modifier;
static uint8_t s_keycode;
static bool s_enabled;
static bool s_held;
static char s_combo_text[HID_COMBO_CAPACITY] = HID_DEFAULT_COMBO;
static usb_hid_mode_t s_mode = USB_HID_MODE_TAP;

static const struct
{
    const char *name;
    uint8_t usage;
} s_key_names[] = {
    {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29},
    {"backspace", 0x2a}, {"tab", 0x2b}, {"space", 0x2c}, {"minus", 0x2d},
    {"equal", 0x2e}, {"left", 0x50}, {"right", 0x4f}, {"down", 0x51},
    {"up", 0x52}, {"home", 0x4a}, {"end", 0x4d}, {"insert", 0x49},
    {"delete", 0x4c}, {"pageup", 0x4b}, {"pagedown", 0x4e},
};

static uint8_t lookup_key(const char *name, size_t length)
{
    if (length == 1)
    {
        const char c = (char)tolower((unsigned char)name[0]);
        if (c >= 'a' && c <= 'z')
        {
            return (uint8_t)(0x04 + (c - 'a'));
        }
        if (c >= '1' && c <= '9')
        {
            return (uint8_t)(0x1e + (c - '1'));
        }
        if (c == '0')
        {
            return 0x27;
        }
    }
    if (length >= 2 && length <= 3 && tolower((unsigned char)name[0]) == 'f')
    {
        int index = 0;
        bool digits = length > 1;
        for (size_t i = 1; i < length; ++i)
        {
            if (!isdigit((unsigned char)name[i]))
            {
                digits = false;
                break;
            }
            index = index * 10 + (name[i] - '0');
        }
        if (digits && index >= 1 && index <= 24)
        {
            // F1..F12 are contiguous; F13..F24 sit at 0x68 in the usage table.
            return index <= 12 ? (uint8_t)(0x3a + index - 1)
                               : (uint8_t)(0x68 + index - 13);
        }
    }
    for (size_t i = 0; i < sizeof(s_key_names) / sizeof(s_key_names[0]); ++i)
    {
        if (strlen(s_key_names[i].name) == length &&
            strncmp(s_key_names[i].name, name, length) == 0)
        {
            return s_key_names[i].usage;
        }
    }
    return 0;
}

/* Parses "<mod>+<mod>+<key>". Modifiers are optional but at least one key is
 * required, so a typo cannot silently turn push-to-talk into a no-op. */
static esp_err_t parse_combo(const char *text, uint8_t *modifier, uint8_t *keycode)
{
    char buffer[HID_COMBO_CAPACITY];
    size_t length = strlen(text);
    if (length == 0 || length >= sizeof(buffer))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i <= length; ++i)
    {
        buffer[i] = (char)tolower((unsigned char)text[i]);
    }

    uint8_t mods = 0;
    uint8_t key = 0;
    char *cursor = buffer;
    while (cursor && *cursor)
    {
        char *separator = strchr(cursor, '+');
        const size_t token_length = separator ? (size_t)(separator - cursor) : strlen(cursor);
        if (token_length == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (token_length == 4 && strncmp(cursor, "ctrl", 4) == 0)
        {
            mods |= HID_MOD_LCTRL;
        }
        else if (token_length == 7 && strncmp(cursor, "control", 7) == 0)
        {
            mods |= HID_MOD_LCTRL;
        }
        else if (token_length == 5 && strncmp(cursor, "shift", 5) == 0)
        {
            mods |= HID_MOD_LSHIFT;
        }
        else if (token_length == 3 && strncmp(cursor, "alt", 3) == 0)
        {
            mods |= HID_MOD_LALT;
        }
        else if ((token_length == 3 && strncmp(cursor, "win", 3) == 0) ||
                 (token_length == 3 && strncmp(cursor, "gui", 3) == 0) ||
                 (token_length == 5 && strncmp(cursor, "super", 5) == 0) ||
                 (token_length == 4 && strncmp(cursor, "meta", 4) == 0))
        {
            mods |= HID_MOD_LGUI;
        }
        else
        {
            const uint8_t usage = lookup_key(cursor, token_length);
            if (usage == 0 || key != 0)
            {
                return ESP_ERR_INVALID_ARG;
            }
            key = usage;
        }
        cursor = separator ? separator + 1 : NULL;
    }
    if (key == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *modifier = mods;
    *keycode = key;
    return ESP_OK;
}

static bool is_off(const char *text)
{
    return strcmp(text, "off") == 0 || strcmp(text, "none") == 0 ||
           strcmp(text, "disable") == 0 || strcmp(text, "0") == 0;
}

static esp_err_t save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HID_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(handle, HID_NVS_COMBO, s_combo_text);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, HID_NVS_MODE, (uint8_t)s_mode);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Applies a combo to the running configuration. Loading from NVS passes
 * persist=false: rewriting an unchanged value on every boot would burn flash. */
static esp_err_t apply_combo(const char *text, bool persist)
{
    if (!text)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[HID_COMBO_CAPACITY];
    size_t length = strlen(text);
    if (length == 0 || length >= sizeof(normalized))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i <= length; ++i)
    {
        normalized[i] = (char)tolower((unsigned char)text[i]);
    }

    if (is_off(normalized))
    {
        s_enabled = false;
        s_modifier = 0;
        s_keycode = 0;
        snprintf(s_combo_text, sizeof(s_combo_text), "%s", "off");
        return persist ? save() : ESP_OK;
    }

    uint8_t modifier = 0;
    uint8_t keycode = 0;
    const esp_err_t err = parse_combo(normalized, &modifier, &keycode);
    if (err != ESP_OK)
    {
        return err;
    }
    s_modifier = modifier;
    s_keycode = keycode;
    s_enabled = true;
    snprintf(s_combo_text, sizeof(s_combo_text), "%s", normalized);
    return persist ? save() : ESP_OK;
}

esp_err_t usb_hid_set_combo(const char *text)
{
    return apply_combo(text, true);
}

esp_err_t usb_hid_set_mode(const char *text)
{
    usb_hid_mode_t mode;
    if (!text || strcmp(text, "tap") == 0)
    {
        mode = USB_HID_MODE_TAP;
    }
    else if (strcmp(text, "hold") == 0)
    {
        mode = USB_HID_MODE_HOLD;
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == s_mode)
    {
        return ESP_OK;
    }
    s_mode = mode;
    s_held = false;
    return save();
}

const char *usb_hid_combo_text(void)
{
    return s_combo_text;
}

const char *usb_hid_mode_name(void)
{
    return s_mode == USB_HID_MODE_HOLD ? "hold" : "tap";
}

bool usb_hid_enabled(void)
{
    return s_enabled && s_keycode != 0;
}

bool usb_hid_ready(void)
{
    return tud_mounted() && tud_hid_ready();
}

static bool send_report(bool pressed)
{
    if (!usb_hid_ready())
    {
        return false;
    }
    uint8_t keys[6] = {0};
    if (pressed)
    {
        keys[0] = s_keycode;
    }
    return tud_hid_keyboard_report(0, pressed ? s_modifier : 0, pressed ? keys : NULL);
}

static bool send_report_retry(bool pressed)
{
    for (int attempt = 0; attempt < HID_READY_RETRY; ++attempt)
    {
        if (send_report(pressed))
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

esp_err_t usb_hid_tap(void)
{
    if (!usb_hid_enabled())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!send_report_retry(true))
    {
        return ESP_ERR_INVALID_STATE;
    }
    // The host polls an interrupt endpoint; keep the press visible for at least
    // one polling interval, or Windows can miss the keystroke entirely.
    vTaskDelay(pdMS_TO_TICKS(HID_RELEASE_GAP_MS));
    return send_report_retry(false) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t usb_hid_start(void)
{
    if (!usb_hid_enabled())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mode == USB_HID_MODE_HOLD)
    {
        if (s_held)
        {
            return ESP_OK;
        }
        if (!send_report_retry(true))
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_held = true;
        return ESP_OK;
    }
    return usb_hid_tap();
}

esp_err_t usb_hid_stop(void)
{
    if (s_mode != USB_HID_MODE_HOLD)
    {
        if (!usb_hid_enabled())
        {
            return ESP_ERR_INVALID_STATE;
        }
        // TAP mode is a toggle: the host app started from one press and only
        // stops on the next one. Returning without sending here was leaving
        // dictation running after the device had already stopped listening.
        return usb_hid_tap();
    }
    return usb_hid_release();
}

esp_err_t usb_hid_release(void)
{
    if (s_mode != USB_HID_MODE_HOLD || !s_held)
    {
        return ESP_OK;
    }
    s_held = false;
    return send_report_retry(false) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t usb_hid_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES)
    {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open(HID_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        // Nothing saved yet: keep the built-in default without writing flash.
        return apply_combo(HID_DEFAULT_COMBO, false);
    }

    char stored[HID_COMBO_CAPACITY] = {0};
    size_t length = sizeof(stored);
    err = nvs_get_str(handle, HID_NVS_COMBO, stored, &length);
    uint8_t mode = USB_HID_MODE_TAP;
    const esp_err_t mode_err = nvs_get_u8(handle, HID_NVS_MODE, &mode);
    nvs_close(handle);

    if (mode_err == ESP_OK && mode <= USB_HID_MODE_HOLD)
    {
        s_mode = (usb_hid_mode_t)mode;
    }
    if (err != ESP_OK)
    {
        return apply_combo(HID_DEFAULT_COMBO, false);
    }
    if (apply_combo(stored, false) != ESP_OK)
    {
        ESP_LOGW(TAG, "Stored combo '%s' is invalid; using %s", stored, HID_DEFAULT_COMBO);
        return apply_combo(HID_DEFAULT_COMBO, false);
    }
    return ESP_OK;
}
