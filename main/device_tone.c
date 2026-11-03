#include "device_tone.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "box2.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "device_tone";

enum
{
    TONE_RATE = 48000,
    // Short on purpose: the cue plays while the microphone is still muted, so
    // every millisecond here delays the moment the user is actually recorded.
    TONE_MS = 35,
    TONE_FRAMES = TONE_RATE / 1000 * TONE_MS,
    TONE_FADE_FRAMES = TONE_RATE / 200, /* 5 ms */
    TONE_AMPLITUDE = 4200,
    TONE_VOLUME = 45,
    // Silence padding around the amplifier toggle. Without it the PA switches
    // while the DAC still has energy, which is heard as a click.
    PAD_FRAMES = TONE_RATE / 100, /* 10 ms */
};

// Stereo interleaved: [L0, R0, L1, R1, ...]. Both channels carry the same
// sample; see box2_audio_write() for why the speaker must stay stereo.
// Neither buffer may be const: the codec layer runs its software volume in
// place on the caller's memory, so read-only data would fault.
static int16_t s_tone[TONE_FRAMES * 2];
static int16_t s_silence[PAD_FRAMES * 2];

static void fill(int16_t *buffer, int frames, int frequency)
{
    const float step = 6.28318531f * (float)frequency / (float)TONE_RATE;
    for (int i = 0; i < frames; ++i)
    {
        float envelope = 1.0f;
        if (i < TONE_FADE_FRAMES)
        {
            envelope = (float)i / (float)TONE_FADE_FRAMES;
        }
        else if (i >= frames - TONE_FADE_FRAMES)
        {
            envelope = (float)(frames - i) / (float)TONE_FADE_FRAMES;
        }
        const int16_t sample =
            (int16_t)(sinf(step * (float)i) * (float)TONE_AMPLITUDE * envelope);
        buffer[2 * i] = sample;
        buffer[2 * i + 1] = sample;
    }
}

static void write_frames(const int16_t *frames, int count)
{
    (void)box2_audio_write(frames, (size_t)count);
}

static void play(int first, int second)
{
    if (box2_board_set_speaker_enabled(true) != ESP_OK)
    {
        return;
    }
    // The volume register starts at zero, so the amplifier is toggled while the
    // DAC is already sitting at silence, then the level is raised.
    write_frames(s_silence, PAD_FRAMES);
    (void)box2_audio_set_output_volume(TONE_VOLUME);
    write_frames(s_silence, PAD_FRAMES);

    fill(s_tone, TONE_FRAMES, first);
    write_frames(s_tone, TONE_FRAMES);
    if (second > 0)
    {
        fill(s_tone, TONE_FRAMES, second);
        write_frames(s_tone, TONE_FRAMES);
    }

    // Drop the level before the amplifier opens, and give the DMA time to push
    // the padding out, so the switch never lands on a non-zero output.
    write_frames(s_silence, PAD_FRAMES);
    (void)box2_audio_set_output_volume(0);
    vTaskDelay(pdMS_TO_TICKS(15));
    (void)box2_board_set_speaker_enabled(false);
}

void device_tone_start(void)
{
    play(880, 1320);
}

void device_tone_stop(void)
{
    play(1320, 660);
}

void device_tone_error(void)
{
    play(320, 0);
}

esp_err_t device_tone_self_test(void)
{
    if (box2_audio_sample_rate() == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Playing start cue");
    device_tone_start();
    return ESP_OK;
}
