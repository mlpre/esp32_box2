#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "box2_audio.h"
#include "box2_board.h"
#include "box2_lcd.h"
#include "box2_motion.h"
#include "box2_storage.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
static const char *TAG = "box2_demo";
typedef struct {
    bool initialized;
    bool scan_ok;
    uint16_t ap_count;
    int8_t strongest_rssi;
    char strongest_ssid[33];
} wifi_test_state_t;
static wifi_test_state_t s_wifi;
static esp_err_t wifi_test_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    s_wifi.initialized = true;
    return ESP_OK;
}
static esp_err_t wifi_test_scan(void)
{
    if (!s_wifi.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 100,
            .max = 250,
        },
        .home_chan_dwell_time = 30,
    };
    ESP_LOGI(TAG, "Scanning 2.4 GHz Wi-Fi...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        s_wifi.scan_ok = false;
        return err;
    }
    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "get AP count");
    uint16_t read_count = count > 24 ? 24 : count;
    wifi_ap_record_t *records = calloc(read_count ? read_count : 1, sizeof(*records));
    ESP_RETURN_ON_FALSE(records, ESP_ERR_NO_MEM, TAG, "allocate AP records");
    err = esp_wifi_scan_get_ap_records(&read_count, records);
    if (err == ESP_OK) {
        s_wifi.ap_count = count;
        s_wifi.strongest_rssi = read_count ? records[0].rssi : -127;
        snprintf(s_wifi.strongest_ssid, sizeof(s_wifi.strongest_ssid), "%s",
                 read_count ? (const char *)records[0].ssid : "NONE");
        s_wifi.scan_ok = true;
        ESP_LOGI(TAG, "Wi-Fi scan PASS: %u AP(s), strongest '%s' (%d dBm)",
                 count, s_wifi.strongest_ssid, s_wifi.strongest_rssi);
        for (uint16_t i = 0; i < read_count && i < 10; ++i) {
            ESP_LOGI(TAG, "  %2u  ch=%2u  rssi=%4d  %s", i + 1, records[i].primary,
                     records[i].rssi, records[i].ssid);
        }
    } else {
        s_wifi.scan_ok = false;
    }
    free(records);
    return err;
}
static void print_system_info(uint32_t *flash_mb, uint32_t *psram_mb)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    *flash_mb = flash_size / (1024 * 1024);
    *psram_mb = (uint32_t)(psram_size / (1024 * 1024));
    ESP_LOGI(TAG, "ATK-DNESP32S3-BOX2-WIFI hardware test");
    ESP_LOGI(TAG, "chip=%s cores=%u rev=%u flash=%" PRIu32 "MB psram=%" PRIu32 "MB",
             CONFIG_IDF_TARGET, chip_info.cores, chip_info.revision, *flash_mb, *psram_mb);
}
static void update_screen(bool lcd_ok, bool audio_ok, bool board_ok,
                          uint32_t flash_mb, uint32_t psram_mb,
                          const box2_board_state_t *board,
                          const box2_motion_state_t *motion,
                          const box2_storage_state_t *storage, int mic_peak)
{
    if (!lcd_ok) {
        return;
    }
    char text[17][40];
    const char *lines[17];
    for (int i = 0; i < 17; ++i) {
        lines[i] = text[i];
    }
    snprintf(text[0], sizeof(text[0]), "SYS FLASH=%" PRIu32 "M PSRAM=%" PRIu32 "M CPU=240M",
             flash_mb, psram_mb);
    int i2c_count = box2_board_i2c_device_count();
    bool i2c_ok = board_ok && i2c_count >= 3;
    snprintf(text[1], sizeof(text[1]), "I2C %s N=%d ADDR=10,19,20", i2c_ok ? "OK" : "FAIL",
             i2c_count);
    snprintf(text[2], sizeof(text[2]), "XIO %s RAW=%04X SAFE=%s",
             board->expander_outputs_ok ? "OK" : "FAIL", board->xio,
             board->expander_outputs_ok ? "OK" : "FAIL");
    snprintf(text[3], sizeof(text[3]), "LCD OK 240X320 I80 BL=PWM");
    snprintf(text[4], sizeof(text[4]), "AUDIO %s MIC=%d %s", audio_ok ? "OK" : "FAIL",
             mic_peak, audio_ok ? "LIVE" : "OFF");
    snprintf(text[5], sizeof(text[5]), "WIFI %s AP=%u RSSI=%dDBM", s_wifi.scan_ok ? "OK" : "FAIL",
             s_wifi.ap_count, s_wifi.strongest_rssi);
    snprintf(text[6], sizeof(text[6]), "SSID %.31s", s_wifi.strongest_ssid);
    snprintf(text[7], sizeof(text[7]), "BAT %dMV %d%% %s ADC=%d", board->battery_mv_estimate,
             board->battery_percent, board->charging ? "USB" : "BAT", board->battery_raw);
    snprintf(text[8], sizeof(text[8]), "KEY L%d Q%d M%d R%d RAW=%d%d%d%d", board->left_pressed,
             board->q_pressed, board->middle_pressed, board->right_pressed,
             board->left_level, board->q_level, board->middle_level, board->right_level);
    snprintf(text[9], sizeof(text[9]), "ACC %s ID=%02X ORI=%s",
             motion->detected ? "OK" : "FAIL", motion->who_am_i,
             motion->orientation ? motion->orientation : "NONE");
    snprintf(text[10], sizeof(text[10]), "ACC X=%+5dMG RAW=%+6d", motion->x_mg, motion->raw_x);
    snprintf(text[11], sizeof(text[11]), "ACC Y=%+5dMG RAW=%+6d", motion->y_mg, motion->raw_y);
    snprintf(text[12], sizeof(text[12]), "ACC Z=%+5dMG RAW=%+6d", motion->z_mg, motion->raw_z);
    snprintf(text[13], sizeof(text[13]), "SD %s NAME=%s RW=%s",
             storage->mounted ? "OK" : "NO CARD", storage->name[0] ? storage->name : "-",
             storage->read_write_ok ? "OK" : "FAIL");
    snprintf(text[14], sizeof(text[14]), "SD CAP=%" PRIu32 "M FAT=%" PRIu32 "M FREE=%" PRIu32 "M",
             storage->capacity_mb, storage->total_mb, storage->free_mb);
    snprintf(text[15], sizeof(text[15]), "SD SPI S17 MO16 MI18 CS15 25M");
    snprintf(text[16], sizeof(text[16]), "TONE L440 Q660 M880 R1040");
    int meter = mic_peak * 100 / 6000;
    if (meter > 100) meter = 100;
    esp_err_t err = box2_lcd_show_lines("BOX2 COMPLETE HARDWARE TEST", lines, 17, meter);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD refresh failed: %s", esp_err_to_name(err));
    }
}
void app_main(void)
{
    uint32_t flash_mb = 0;
    uint32_t psram_mb = 0;
    print_system_info(&flash_mb, &psram_mb);
    bool board_ok = box2_board_init() == ESP_OK;
    ESP_LOGI(TAG, "I2C/TCA9555 test: %s", board_ok ? "PASS" : "FAIL");
    bool lcd_ok = box2_lcd_init() == ESP_OK;
    ESP_LOGI(TAG, "LCD/backlight test: %s", lcd_ok ? "PASS" : "FAIL");
    if (lcd_ok) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_lcd_show_color_test());
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    box2_board_state_t board_state = {0};
    if (board_ok) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_board_read_state(&board_state, true));
        ESP_LOGI(TAG, "battery raw=%d estimate=%d mV level=%d%% charging=%d",
                 board_state.battery_raw, board_state.battery_mv_estimate,
                 board_state.battery_percent, board_state.charging);
    }
    box2_motion_state_t motion_state = {0};
    bool motion_ok = board_ok && box2_motion_init(box2_board_i2c_bus()) == ESP_OK;
    if (motion_ok) {
        motion_ok = box2_motion_read(&motion_state) == ESP_OK;
    }
    ESP_LOGI(TAG, "SC7A20 test: %s", motion_ok ? "PASS" : "FAIL");
    box2_storage_state_t storage_state = {0};
    bool storage_ok = box2_storage_test(&storage_state) == ESP_OK;
    ESP_LOGI(TAG, "TF/MicroSD test: %s", storage_ok ? "PASS" : "FAIL");
    bool audio_ok = board_ok && box2_audio_init(box2_board_i2c_bus()) == ESP_OK;
    ESP_LOGI(TAG, "ES8389/I2S test: %s", audio_ok ? "PASS" : "FAIL");
    if (audio_ok) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(523, 180));
        vTaskDelay(pdMS_TO_TICKS(80));
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(784, 220));
    }
    bool wifi_ok = wifi_test_init() == ESP_OK && wifi_test_scan() == ESP_OK;
    ESP_LOGI(TAG, "Wi-Fi radio test: %s", wifi_ok ? "PASS" : "FAIL");
    bool previous_left = false;
    bool previous_middle = false;
    bool previous_right = false;
    bool previous_q = board_state.q_pressed;
    TickType_t last_screen = 0;
    TickType_t last_battery = 0;
    TickType_t last_motion = 0;
    TickType_t last_log = 0;
    int mic_peak = 0;
    ESP_LOGI(TAG, "Interactive mode: L=440Hz, Q=660Hz, M=880Hz, R=1040Hz");
    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool sample_battery = (now - last_battery) >= pdMS_TO_TICKS(5000);
        if (board_ok && box2_board_read_state(&board_state, sample_battery) == ESP_OK &&
            sample_battery) {
            last_battery = now;
        }
        if (motion_ok && (now - last_motion) >= pdMS_TO_TICKS(100)) {
            if (box2_motion_read(&motion_state) != ESP_OK) {
                motion_state.detected = false;
            }
            last_motion = now;
        }
        if (audio_ok) {
            int new_peak = 0;
            if (box2_audio_read_peak(&new_peak) == ESP_OK) {
                mic_peak = new_peak;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (board_state.left_pressed && !previous_left && audio_ok) {
            ESP_LOGI(TAG, "LEFT key: speaker 440 Hz");
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(440, 180));
        }
        if (board_state.middle_pressed && !previous_middle && audio_ok) {
            ESP_LOGI(TAG, "MIDDLE key: speaker 880 Hz");
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(880, 180));
        }
        if (board_state.q_pressed && !previous_q && audio_ok) {
            ESP_LOGI(TAG, "Q key: speaker 660 Hz");
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(660, 180));
        }
        if (board_state.right_pressed && !previous_right && audio_ok) {
            ESP_LOGI(TAG, "RIGHT key: speaker 1040 Hz");
            ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_play_tone(1040, 180));
        }
        previous_left = board_state.left_pressed;
        previous_middle = board_state.middle_pressed;
        previous_right = board_state.right_pressed;
        previous_q = board_state.q_pressed;
        if ((now - last_screen) >= pdMS_TO_TICKS(400)) {
            update_screen(lcd_ok, audio_ok, board_ok, flash_mb, psram_mb,
                          &board_state, &motion_state, &storage_state, mic_peak);
            last_screen = now;
        }
        if ((now - last_log) >= pdMS_TO_TICKS(2000)) {
            last_log = now;
            ESP_LOGI(TAG, "mic=%5d keys=L%d Q%d M%d R%d raw=%d%d%d%d battery=%d%% "
                     "xio=0x%04X acc=%+d,%+d,%+dmg orient=%s sd=%s rw=%s",
                     mic_peak, board_state.left_pressed, board_state.q_pressed,
                     board_state.middle_pressed, board_state.right_pressed,
                     board_state.left_level, board_state.q_level,
                     board_state.middle_level, board_state.right_level,
                     board_state.battery_percent, board_state.xio, motion_state.x_mg,
                     motion_state.y_mg, motion_state.z_mg,
                     motion_state.orientation ? motion_state.orientation : "NONE",
                     storage_state.mounted ? "MOUNTED" : "NO_CARD",
                     storage_state.read_write_ok ? "PASS" : "FAIL");
        }
    }
}
