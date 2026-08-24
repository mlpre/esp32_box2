#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct
{
    bool mounted;
    uint32_t capacity_mb;
    uint32_t total_mb;
    uint32_t free_mb;
    char name[9];
    esp_err_t error;
} box2_storage_state_t;
esp_err_t box2_storage_mount(box2_storage_state_t *state);
esp_err_t box2_storage_refresh(box2_storage_state_t *state);
esp_err_t box2_storage_unmount(void);
const char *box2_storage_mount_point(void);
