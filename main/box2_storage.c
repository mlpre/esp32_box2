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
static esp_err_t test_file(void)
{
    static const char expected[] = "BOX2 SD READ WRITE PASS\n";
    const char *path = SD_MOUNT_POINT "/box2_test.tmp";
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    size_t written = fwrite(expected, 1, sizeof(expected), file);
    int close_error = fclose(file);
    if (written != sizeof(expected) || close_error != 0) {
        remove(path);
        return ESP_FAIL;
    }
    char actual[sizeof(expected)] = {0};
    file = fopen(path, "rb");
    if (!file) {
        remove(path);
        return ESP_FAIL;
    }
    size_t read = fread(actual, 1, sizeof(actual), file);
    close_error = fclose(file);
    int remove_error = remove(path);
    return read == sizeof(expected) && close_error == 0 && remove_error == 0 &&
                   memcmp(actual, expected, sizeof(expected)) == 0
               ? ESP_OK
               : ESP_FAIL;
}
esp_err_t box2_storage_test(box2_storage_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is null");
    memset(state, 0, sizeof(*state));
    if (!s_bus_initialized) {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = BOX2_SD_MOSI,
            .miso_io_num = BOX2_SD_MISO,
            .sclk_io_num = BOX2_SD_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
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
    if (err != ESP_OK) {
        state->error = err;
        ESP_LOGW(TAG, "TF card mount FAIL: %s", esp_err_to_name(err));
        return err;
    }
    state->mounted = true;
    snprintf(state->name, sizeof(state->name), "%.8s", s_card->cid.name);
    uint64_t capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    state->capacity_mb = (uint32_t)(capacity / (1024 * 1024));
    uint64_t total = 0;
    uint64_t free = 0;
    err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free);
    if (err == ESP_OK) {
        state->total_mb = (uint32_t)(total / (1024 * 1024));
        state->free_mb = (uint32_t)(free / (1024 * 1024));
    }
    esp_err_t rw_err = test_file();
    state->read_write_ok = rw_err == ESP_OK;
    state->error = err != ESP_OK ? err : rw_err;
    ESP_LOGI(TAG, "TF card %s: name=%s capacity=%" PRIu32 "MB total=%" PRIu32
             "MB free=%" PRIu32 "MB read/write=%s",
             state->error == ESP_OK ? "PASS" : "FAIL", state->name,
             state->capacity_mb, state->total_mb, state->free_mb,
             state->read_write_ok ? "PASS" : "FAIL");
    return state->error;
}
