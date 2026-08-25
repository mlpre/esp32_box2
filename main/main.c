#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "box2.h"
#include "box2_config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "radio_screen.h"
#include "radio_stream.h"
#include "weather_service.h"

#define WIFI_CONNECTED_BIT BIT0
#define UI_REFRESH_MS 300
#define KEY_POLL_MS 30
#define KEY_LONG_PRESS_MS 2000
#define WAKE_PIN_STABLE_MS 150
#define BATTERY_REFRESH_MS 10000
#define VOLUME_STEP 5

static const char *TAG = "box2_radio";
static EventGroupHandle_t s_wifi_events;
static char s_ip_address[16] = "-";

static esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        snprintf(s_ip_address, sizeof(s_ip_address), "-");
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&got_ip->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP=%s", s_ip_address);
    }
}
static esp_err_t wifi_init(void)
{
    ESP_RETURN_ON_FALSE(CONFIG_BOX2_WIFI_SSID[0] != '\0', ESP_ERR_INVALID_STATE,
                        TAG, "Wi-Fi SSID is empty; configure it in menuconfig");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize TCP/IP");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_FAIL, TAG,
                        "create Wi-Fi station interface");

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "create Wi-Fi event group");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "register IP event handler");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_BOX2_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_BOX2_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG,
                        "set Wi-Fi credentials");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "disable Wi-Fi power saving for audio streaming");

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t time_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&time_config), TAG,
                        "initialize network time");
    return ESP_OK;
}

static bool wifi_is_connected(void)
{
    return s_wifi_events &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

static void power_off_after_key_release(void)
{
    ESP_LOGI(TAG, "Powering off; press key 3 to wake");
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_lcd_set_backlight(0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_audio_set_output_volume(0));

    box2_board_state_t state = {0};
    do
    {
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    } while (box2_board_read_state(&state, false) == ESP_OK &&
             state.middle_pressed);

    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_board_power_off());

    /* TCA9555 INT is open-drain. Keep its pull-up alive in the RTC domain;
       a normal GPIO pull-up is lost in deep sleep and causes an immediate
       false wake while USB is attached. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_init(BOX2_TCA9555_INT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_set_direction(
        BOX2_TCA9555_INT, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_pulldown_dis(BOX2_TCA9555_INT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_pullup_en(BOX2_TCA9555_INT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_pd_config(
        ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));

    TickType_t wake_pin_high_since = 0;
    while (true)
    {
        /* Reading the expander inputs clears any pending INT condition. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(box2_board_read_state(&state, false));
        TickType_t now = xTaskGetTickCount();
        if (!state.middle_pressed && rtc_gpio_get_level(BOX2_TCA9555_INT) != 0)
        {
            if (wake_pin_high_since == 0)
            {
                wake_pin_high_since = now;
            }
            if ((now - wake_pin_high_since) >=
                pdMS_TO_TICKS(WAKE_PIN_STABLE_MS))
            {
                break;
            }
        }
        else
        {
            wake_pin_high_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Wake pin stable high; entering deep sleep");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_ext1_wakeup_io(
        1ULL << BOX2_TCA9555_INT, ESP_EXT1_WAKEUP_ANY_LOW));
    esp_deep_sleep_start();
}

static void handle_keys(const box2_board_state_t *board, radio_status_t *radio,
                        TickType_t now)
{
    /* Physical order from left to right: q, left, middle, right. */
    static bool previous_left;
    static bool previous_q;
    static bool previous_middle;
    static bool previous_right;
    static bool q_long_press_triggered;
    static bool middle_long_press_triggered;
    static TickType_t q_press_started;
    static TickType_t middle_press_started;

    if (board->left_pressed && !previous_left)
    {
        radio_stream_set_volume(radio->volume_percent - VOLUME_STEP);
    }
    if (board->right_pressed && !previous_right)
    {
        radio_stream_next();
    }
    if (board->q_pressed && !previous_q)
    {
        q_press_started = now;
        q_long_press_triggered = false;
    }
    if (board->q_pressed && !q_long_press_triggered &&
        (now - q_press_started) >= pdMS_TO_TICKS(KEY_LONG_PRESS_MS))
    {
        radio_stream_request_directory_update();
        q_long_press_triggered = true;
    }
    if (!board->q_pressed && previous_q && !q_long_press_triggered)
    {
        radio_stream_previous();
    }
    if (board->middle_pressed && !previous_middle)
    {
        middle_press_started = now;
        middle_long_press_triggered = false;
    }
    if (board->middle_pressed && !middle_long_press_triggered &&
        (now - middle_press_started) >= pdMS_TO_TICKS(KEY_LONG_PRESS_MS))
    {
        middle_long_press_triggered = true;
        power_off_after_key_release();
    }
    if (!board->middle_pressed && previous_middle &&
        !middle_long_press_triggered)
    {
        radio_stream_set_volume(radio->volume_percent + VOLUME_STEP);
    }

    previous_left = board->left_pressed;
    previous_q = board->q_pressed;
    previous_middle = board->middle_pressed;
    previous_right = board->right_pressed;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting standalone BOX2 Internet Radio, wake causes=0x%08lx",
             (unsigned long)esp_sleep_get_wakeup_causes());
    ESP_ERROR_CHECK(box2_board_init());
    ESP_ERROR_CHECK(box2_lcd_init());
    ESP_ERROR_CHECK(box2_lcd_set_backlight(100));
    ESP_ERROR_CHECK(box2_audio_init(box2_board_i2c_bus()));
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(weather_service_init());
    ESP_ERROR_CHECK(radio_stream_init());

    bool wifi_configured = CONFIG_BOX2_WIFI_SSID[0] != '\0';
    if (wifi_configured)
    {
        ESP_ERROR_CHECK(wifi_init());
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi is not configured. Run idf.py menuconfig and open "
                      "'BOX2 Internet Radio'.");
    }

    box2_board_state_t board = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(box2_board_read_state(&board, true));
    bool radio_started = false;
    bool weather_started = false;
    TickType_t last_ui = 0;
    TickType_t last_battery = xTaskGetTickCount();

    while (true)
    {
        TickType_t now = xTaskGetTickCount();
        bool refresh_battery = (now - last_battery) >= pdMS_TO_TICKS(BATTERY_REFRESH_MS);
        if (box2_board_read_state(&board, refresh_battery) == ESP_OK && refresh_battery)
        {
            last_battery = now;
        }

        bool connected = wifi_is_connected();
        if (connected && !radio_started)
        {
            ESP_ERROR_CHECK(radio_stream_start());
            radio_started = true;
        }
        if (connected && !weather_started)
        {
            weather_started = weather_service_start() == ESP_OK;
        }

        radio_status_t radio = {0};
        radio_stream_get_status(&radio);
        handle_keys(&board, &radio, now);

        if ((now - last_ui) >= pdMS_TO_TICKS(UI_REFRESH_MS))
        {
            radio_stream_get_status(&radio);
            weather_status_t weather = {0};
            weather_service_get_status(&weather);
            radio_screen_data_t screen = {
                .wifi_configured = wifi_configured,
                .wifi_connected = connected,
                .wifi_ssid = wifi_configured ? CONFIG_BOX2_WIFI_SSID : "-",
                .ip_address = connected ? s_ip_address : "-",
                .battery_percent = board.battery_percent,
                .charging = board.charging,
                .radio = radio,
                .weather = weather,
            };
            ESP_ERROR_CHECK_WITHOUT_ABORT(radio_screen_show(&screen));
            last_ui = now;
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}
