#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct
{
    uint32_t read_errors;
    // Payload bytes supplied to UAC, including silence; not a host receipt counter.
    uint32_t bytes_sent;
    // Latest capture peak after DC removal, before local/host mute (0..32768).
    // UAC captures only while the host opens the recording stream; this
    // snapshot stays unchanged when the stream is closed or disconnected.
    int peak;
} usb_microphone_stats_t;

// Call once from app initialization, after the codec is opened at 48 kHz.
// UAC owns microphone reads until reboot; other tasks must not read PCM.
// Returning to USB Serial/JTAG requires a reboot into serial mode.
esp_err_t usb_microphone_start(void);
// Permanently stop capture and detach USB for power-off/standby. A subsequent
// power-on restarts the application; this is not a UAC teardown/reinit API.
void usb_microphone_prepare_power_off(void);
// Local mute and host mute are independent; either one silences the stream.
void usb_microphone_set_muted(bool muted);
// Returns the effective mute state (local OR host).
bool usb_microphone_is_muted(void);
int usb_microphone_peak(void);
void usb_microphone_get_stats(usb_microphone_stats_t *stats);
