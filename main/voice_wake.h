#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Reported by voice_wake_take_events(), one bit per occurrence.
enum
{
    VOICE_WAKE_START = 1u << 0, // Wake word heard: begin dictation.
    VOICE_WAKE_STOP = 1u << 1,  // Silence long enough: end dictation.
};

// Loads the wake word model from the "model" partition and starts the always-on
// detector. Call after mic_stream_start(): the detector consumes 16 kHz PCM.
esp_err_t voice_wake_start(void);

// False when no model is present or the front end failed to allocate. The
// push-to-talk button keeps working either way.
bool voice_wake_available(void);

// Keeps the detector in sync with the dictation state. While listening the
// detector watches for end of speech instead of the wake word, so the wake
// word cannot restart a session that is already running.
void voice_wake_set_listening(bool listening);

// Ignores detection for `ms`. Used around the cue tones, which are played
// through the speaker sitting next to the microphone.
void voice_wake_blank(uint32_t ms);

// Returns and clears the pending event bits. Poll from the main loop.
uint32_t voice_wake_take_events(void);
