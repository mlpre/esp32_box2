#include "usb_mode.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "device_tone.h"
#include "esp_private/periph_ctrl.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "usb_hid.h"

#define USB_MODE_NVS_NAMESPACE "box2_usb"
#define USB_MODE_NVS_KEY "mode"
#define COMMAND_LINE_SIZE 64

/* s_mode describes the running boot, never a not-yet-applied USB personality.
 * NVS is updated only by an explicit switch; a missing key defaults to SERIAL.
 */
static usb_mode_t s_mode = USB_MODE_SERIAL;
static bool s_initialized;
static bool s_switch_in_progress;
static bool s_commands_enabled = true;
static uint32_t s_commands_generation;
static TaskHandle_t s_command_task;
static portMUX_TYPE s_switch_lock = portMUX_INITIALIZER_UNLOCKED;

static bool valid_mode(usb_mode_t mode)
{
    return mode == USB_MODE_SERIAL || mode == USB_MODE_MICROPHONE;
}

void usb_mode_set_commands_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_switch_lock);
    if (s_commands_enabled != enabled)
    {
        s_commands_enabled = enabled;
        ++s_commands_generation;
    }
    portEXIT_CRITICAL(&s_switch_lock);
}

static bool commands_snapshot(uint32_t *generation)
{
    portENTER_CRITICAL(&s_switch_lock);
    bool enabled = s_commands_enabled;
    if (generation)
    {
        *generation = s_commands_generation;
    }
    portEXIT_CRITICAL(&s_switch_lock);
    return enabled;
}

void usb_mode_restore_serial_phy(void)
{
    // S3's CPU-only restart does not reset the RTC USB mux set by TinyUSB.
    // Match usb_serial_jtag_hal_phy_set_external(NULL, false), also restoring
    // the default pull resistors if an OTG driver previously overrode them.
    PERIPH_RCC_ATOMIC()
    {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_phy_enable_pad(true);
}

void usb_mode_disconnect_serial_phy(void)
{
    usb_serial_jtag_ll_phy_enable_pad(false);
    // Let the host discard its previous USB configuration and address.
    vTaskDelay(pdMS_TO_TICKS(200));
}

esp_err_t usb_mode_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }
    usb_mode_restore_serial_phy();
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open(USB_MODE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        s_initialized = true;
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }
    uint8_t saved = USB_MODE_SERIAL;
    err = nvs_get_u8(handle, USB_MODE_NVS_KEY, &saved);
    nvs_close(handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        return err;
    }
    if (err == ESP_OK && valid_mode((usb_mode_t)saved))
    {
        s_mode = (usb_mode_t)saved;
    }
    s_initialized = true;
    return ESP_OK;
}

usb_mode_t usb_mode_current(void)
{
    return s_mode;
}

const char *usb_mode_name(usb_mode_t mode)
{
    switch (mode)
    {
    case USB_MODE_SERIAL:
        return "USB SERIAL";
    case USB_MODE_MICROPHONE:
        return "USB MICROPHONE";
    default:
        return "UNKNOWN";
    }
}

esp_err_t usb_mode_switch(usb_mode_t mode)
{
    if (!valid_mode(mode))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == s_mode)
    {
        return ESP_OK;
    }
    // An on-device key and a serial command may request a switch concurrently.
    portENTER_CRITICAL(&s_switch_lock);
    bool busy = s_switch_in_progress;
    if (!busy)
    {
        s_switch_in_progress = true;
    }
    portEXIT_CRITICAL(&s_switch_lock);
    if (busy)
    {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(USB_MODE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, USB_MODE_NVS_KEY, (uint8_t)mode);
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK)
    {
        portENTER_CRITICAL(&s_switch_lock);
        s_switch_in_progress = false;
        portEXIT_CRITICAL(&s_switch_lock);
        return err;
    }
    // Allows the screen and pending console response to reach the user.
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

static void print_help(void)
{
    printf("\nBOX2 USB mode commands:\n"
           "  usb mic          - save microphone mode and restart\n"
           "  usb serial       - save serial mode and restart\n"
           "  usb status       - show current mode\n"
           "  ptt              - show the push-to-talk key\n"
           "  ptt win+h        - set the key the device sends to the host\n"
           "  ptt mode tap     - one press per talk (toggle hotkeys)\n"
           "  ptt mode hold    - press on start, release on stop\n"
           "  ptt off          - stop sending any key\n"
           "  beep             - play the cue (microphone mode only)\n"
           "  help             - show this help\n"
           "Hold Q for 2 seconds to switch USB mode.\n"
           "Keys are stored in NVS and apply to the microphone mode.\n");
}

static void print_ptt(void)
{
    printf("Push-to-talk: key=%s, mode=%s, %s\n", usb_hid_combo_text(),
           usb_hid_mode_name(), usb_hid_enabled() ? "enabled" : "disabled");
}

/* `ptt` is the only way to change the hotkey: the microphone personality has no
 * console of its own, so this command configures the next microphone boot. */
static void handle_ptt(const char *argument)
{
    if (*argument == '\0' || strcmp(argument, "status") == 0)
    {
        print_ptt();
        return;
    }
    if (strncmp(argument, "mode ", 5) == 0)
    {
        if (usb_hid_set_mode(argument + 5) == ESP_OK)
        {
            printf("Push-to-talk mode: %s\n", usb_hid_mode_name());
        }
        else
        {
            printf("Unknown mode. Use tap or hold.\n");
        }
        return;
    }
    if (usb_hid_set_combo(argument) == ESP_OK)
    {
        printf("Push-to-talk key: %s\n", usb_hid_combo_text());
    }
    else
    {
        printf("Invalid key. Examples: win+h, ctrl+shift+d, f13, enter, off.\n");
    }
}

static void execute_command(char *line)
{
    if (!commands_snapshot(NULL))
    {
        return;
    }
    // Normalize case/whitespace in place without allocating in the task loop.
    size_t length = 0;
    bool space = false;
    for (size_t i = 0; line[i]; ++i)
    {
        unsigned char c = (unsigned char)line[i];
        if (isspace(c))
        {
            space = length != 0;
            continue;
        }
        if (space)
        {
            line[length++] = ' ';
            space = false;
        }
        line[length++] = (char)tolower(c);
    }
    line[length] = '\0';
    if (!length)
    {
        return;
    }
    if (!strcmp(line, "help"))
    {
        print_help();
        return;
    }
    if (strncmp(line, "ptt", 3) == 0 && (line[3] == '\0' || line[3] == ' '))
    {
        handle_ptt(line[3] == '\0' ? "" : &line[4]);
        return;
    }
    if (!strcmp(line, "beep"))
    {
        const esp_err_t err = device_tone_self_test();
        if (err == ESP_OK)
        {
            printf("Cue played.\n");
        }
        else if (err == ESP_ERR_INVALID_STATE)
        {
            printf("Audio starts only in microphone mode.\n");
        }
        else
        {
            printf("Cue failed: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (!strcmp(line, "usb status"))
    {
        printf("USB mode: %s; hold Q for 2 seconds to switch.\n",
               usb_mode_name(usb_mode_current()));
        return;
    }
    usb_mode_t target;
    if (!strcmp(line, "usb mic"))
    {
        target = USB_MODE_MICROPHONE;
    }
    else if (!strcmp(line, "usb serial"))
    {
        target = USB_MODE_SERIAL;
    }
    else
    {
        printf("Unknown command. Enter help.\n");
        return;
    }
    if (target == usb_mode_current())
    {
        printf("Already in %s mode.\n", usb_mode_name(target));
        return;
    }
    printf("Saving %s mode; restarting after commit...\n", usb_mode_name(target));
    esp_err_t err = usb_mode_switch(target);
    if (err != ESP_OK)
    {
        printf("USB mode unchanged: %s\n", esp_err_to_name(err));
    }
}

static void serial_command_task(void *arg)
{
    (void)arg;
    char line[COMMAND_LINE_SIZE];
    size_t length = 0;
    bool overflow = false;
    uint32_t generation;
    (void)commands_snapshot(&generation);
    print_help();
    while (true)
    {
        char input[32];
        ssize_t count = read(STDIN_FILENO, input, sizeof(input));
        for (ssize_t i = 0; i < count; ++i)
        {
            uint32_t current_generation;
            bool enabled = commands_snapshot(&current_generation);
            if (current_generation != generation)
            {
                length = 0;
                overflow = false;
                generation = current_generation;
            }
            if (!enabled)
            {
                // Continue reading during standby; never queue disabled input.
                continue;
            }
            unsigned char c = (unsigned char)input[i];
            if (c == '\r' || c == '\n')
            {
                if (overflow)
                {
                    printf("Command too long. Enter help.\n");
                }
                else if (length)
                {
                    line[length] = '\0';
                    execute_command(line);
                }
                length = 0;
                overflow = false;
            }
            else if (!overflow && (c == '\b' || c == 0x7f))
            {
                if (length)
                {
                    --length;
                }
            }
            else if (!overflow && (isprint(c) || c == '\t'))
            {
                if (length < sizeof(line) - 1)
                {
                    line[length++] = (char)c;
                }
                else
                {
                    // Discard the whole line, never execute its truncated tail.
                    overflow = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t usb_mode_start_serial_commands(void)
{
    if (!s_initialized || s_mode != USB_MODE_SERIAL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_command_task)
    {
        return ESP_OK;
    }
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return ESP_FAIL;
    }
    // Keep CR and LF visible to the parser, including CR-only terminal input.
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    return xTaskCreate(serial_command_task, "usb_commands", 3072, NULL,
                       tskIDLE_PRIORITY + 1, &s_command_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
