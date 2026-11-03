#pragma once

#include "esp_err.h"

/* Short audible cues for the microphone personality. The BOX2 amplifier is off
 * in normal operation, so each cue enables it, plays, and disables it again.
 *
 * Blocking: a cue lasts roughly 150 ms and holds the expander mutex while the
 * amplifier is toggled. Call from one task (the device loop) or from the serial
 * command task, never from the UAC capture callback.
 */
void device_tone_start(void);
void device_tone_stop(void);
void device_tone_error(void);

/* Plays the start cue, for the serial `beep` self test. */
esp_err_t device_tone_self_test(void);
