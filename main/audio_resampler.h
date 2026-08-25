#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct audio_resampler audio_resampler_t;

typedef esp_err_t (*audio_resampler_write_cb_t)(const int16_t *samples,
                                                size_t sample_count,
                                                void *context);

audio_resampler_t *audio_resampler_create(uint32_t output_rate);
void audio_resampler_destroy(audio_resampler_t *resampler);

esp_err_t audio_resampler_process(audio_resampler_t *resampler,
                                  const int16_t *pcm,
                                  size_t frame_count,
                                  uint8_t channels,
                                  uint32_t source_rate,
                                  audio_resampler_write_cb_t write_cb,
                                  void *write_context);
