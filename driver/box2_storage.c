#include "box2_storage.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "box2_config.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#define SD_MOUNT_POINT "/sdcard"
static const char *TAG = "box2_storage";
static bool s_bus_initialized;
static sdmmc_card_t *s_card;
esp_err_t box2_storage_mount(box2_storage_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is null");
    memset(state, 0, sizeof(*state));
    if (s_card)
    {
        return box2_storage_refresh(state);
    }
    if (!s_bus_initialized)
    {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = BOX2_SD_MOSI,
            .miso_io_num = BOX2_SD_MISO,
            .sclk_io_num = BOX2_SD_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
        {
            state->error = err;
            return err;
        }
        s_bus_initialized = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 25000;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI3_HOST;
    slot_config.gpio_cs = BOX2_SD_CS;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK)
    {
        state->error = err;
        ESP_LOGW(TAG, "TF card mount FAIL: %s", esp_err_to_name(err));
        return err;
    }
    return box2_storage_refresh(state);
}

esp_err_t box2_storage_refresh(box2_storage_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is null");
    ESP_RETURN_ON_FALSE(s_card, ESP_ERR_INVALID_STATE, TAG, "storage is not mounted");
    memset(state, 0, sizeof(*state));
    state->mounted = true;
    snprintf(state->name, sizeof(state->name), "%.8s", s_card->cid.name);
    uint64_t capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    state->capacity_mb = (uint32_t)(capacity / (1024 * 1024));
    uint64_t total = 0;
    uint64_t free = 0;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free);
    if (err == ESP_OK)
    {
        state->total_mb = (uint32_t)(total / (1024 * 1024));
        state->free_mb = (uint32_t)(free / (1024 * 1024));
    }
    state->error = err;
    ESP_LOGI(TAG, "TF card mounted: name=%s capacity=%" PRIu32 "MB total=%" PRIu32 "MB free=%" PRIu32 "MB",
             state->name, state->capacity_mb, state->total_mb, state->free_mb);
    return state->error;
}

esp_err_t box2_storage_unmount(void)
{
    if (!s_card)
    {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err == ESP_OK)
    {
        s_card = NULL;
    }
    return err;
}

const char *box2_storage_mount_point(void)
{
    return SD_MOUNT_POINT;
}
