#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The device exposes a boot keyboard next to the UAC microphone, so a key
 * press on the BOX2 can start dictation inside any host application instead of
 * relying on the user to click the app's own microphone button.
 *
 * The combo is stored in NVS because the microphone personality has no console
 * of its own: configure it once in serial mode, then switch back.
 */

typedef enum
{
    USB_HID_MODE_TAP = 0, /* One short press per call. Matches toggle hotkeys. */
    USB_HID_MODE_HOLD,    /* Press on start, release on stop. */
} usb_hid_mode_t;

/* Load the saved combo. Must run after NVS is initialized. */
esp_err_t usb_hid_init(void);

/* Accepts "win+h", "ctrl+shift+d", "f13", "enter", "off" (<= 23 characters).
 * Saving writes NVS and takes effect immediately. */
esp_err_t usb_hid_set_combo(const char *text);
esp_err_t usb_hid_set_mode(const char *text);

const char *usb_hid_combo_text(void);
const char *usb_hid_mode_name(void);
bool usb_hid_enabled(void);

/* True while the host is polling the keyboard interface. */
bool usb_hid_ready(void);

/* Push-to-talk edges. TAP mode emits the whole press inside start(), and
 * stop() emits the second press that ends the host's dictation. */
esp_err_t usb_hid_start(void);
esp_err_t usb_hid_stop(void);

/* Release a HOLD-mode key without emitting a new press. Called before the USB
 * device detaches so the host cannot keep the combination asserted. */
esp_err_t usb_hid_release(void);

/* Emit one combo without touching the held state, for `hid` test commands. */
esp_err_t usb_hid_tap(void);
