#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Continuous 48 kHz capture.
 *
 * The microphone used to be read inside the USB callback, so audio only
 * existed while the host had a recording stream open. Wake word detection
 * needs the microphone running all the time, so capture now lives in its own
 * task and hands out two independent streams:
 *
 *   - 48 kHz mono PCM for the USB microphone (usb_microphone.c)
 *   - 16 kHz mono PCM for the speech front end (voice_wake.c), decimated here
 *
 * Splitting the capture out also removes the clock-domain coupling that made
 * long recordings drift: capture is paced by the I2S/DMA clock, USB is paced
 * by the host SOF, and the ring buffer between them absorbs the difference
 * with a one-millisecond water level trim instead of a hard stall.
 */

// Starts the capture task. Call after box2_audio_init_with_sample_rate(48000).
esp_err_t mic_stream_start(void);

// Pulls exactly `samples` samples of 48 kHz PCM. Returns the sample count
// written, which is shorter than requested only when the ring is still empty.
// A short read is padded by repeating the last sample so the host never sees
// a step discontinuity.
size_t mic_stream_read_pcm(int16_t *dst, size_t samples, uint32_t timeout_ms);

// Drops queued 48 kHz audio. Used when the host starts receiving after a mute
// window, so it does not get a burst of stale pre-trigger audio.
void mic_stream_discard(void);

// Pulls exactly `samples` samples of 16 kHz PCM for the wake engine.
size_t mic_stream_read_16k(int16_t *dst, size_t samples, uint32_t timeout_ms);

// Capture diagnostics: latest absolute peak (0..32768) after DC removal, and
// the number of failed I2S reads. A stuck codec shows up as zero peak while
// this counter keeps climbing.
int mic_stream_peak(void);
uint32_t mic_stream_read_errors(void);
