#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum
{
    RADIO_STATE_IDLE = 0,
    RADIO_STATE_LOADING_DIRECTORY,
    RADIO_STATE_CONNECTING,
    RADIO_STATE_BUFFERING,
    RADIO_STATE_PLAYING,
    RADIO_STATE_RETRYING,
    RADIO_STATE_ERROR,
} radio_state_t;

typedef struct
{
    radio_state_t state;
    size_t station_index;
    size_t station_count;
    int volume_percent;
    uint32_t source_sample_rate;
    uint32_t bitrate_kbps;
    uint8_t source_channels;
    uint64_t bytes_received;
    char last_error[64];
} radio_status_t;

esp_err_t radio_stream_init(void);
esp_err_t radio_stream_start(void);
void radio_stream_request_directory_update(void);
void radio_stream_select(size_t station_index);
void radio_stream_next(void);
void radio_stream_previous(void);
esp_err_t radio_stream_set_volume(int volume_percent);
void radio_stream_get_status(radio_status_t *status);
void radio_stream_get_station_name(size_t station_index, char *name,
                                   size_t capacity);
const char *radio_stream_state_name(radio_state_t state);
