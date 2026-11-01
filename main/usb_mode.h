#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    USB_MODE_SERIAL = 0,
    USB_MODE_MICROPHONE = 1,
} usb_mode_t;

/* Read the boot mode from NVS before starting either USB personality. Missing or
 * invalid mode values select SERIAL. Other NVS errors are returned without
 * erasing the partition; current() remains SERIAL as the safe boot fallback.
 * Restores the hardware serial PHY before any logging or TinyUSB startup.
 * No USB driver is installed here and the PHY is not reserved by this module.
 */
esp_err_t usb_mode_init(void);
usb_mode_t usb_mode_current(void);
const char *usb_mode_name(usb_mode_t mode);

/* Use at boot or after disconnecting/stopping TinyUSB in its shutdown handler.
 * esp_restart() resets CPUs but may retain the RTC USB PHY mux on ESP32-S3.
 * This must not run concurrently with an active USB OTG driver.
 */
void usb_mode_restore_serial_phy(void);

/* Disconnect serial USB before starting TinyUSB. Suppress console writes first. */
void usb_mode_disconnect_serial_phy(void);

/* Persist and commit before restarting so the host can enumerate the new USB
 * personality. A successful change does not return. Failures leave the current
 * running mode intact. Selecting the current mode returns ESP_OK immediately.
 * USB drivers must register any required cleanup using a shutdown handler.
 */
esp_err_t usb_mode_switch(usb_mode_t mode);

/* Start a low-priority command task in SERIAL mode only. Reuses the configured
 * USB Serial/JTAG stdin/stdout VFS without installing a driver or changing PHY.
 * Commands: usb mic, usb serial, usb status, help (CR, LF, or CRLF).
 */
esp_err_t usb_mode_start_serial_commands(void);

/* Keep the serial RX task draining while disabling commands in standby or
 * shutdown. A change clears any partial command, so re-enabling starts clean.
 * Calls to usb_mode_switch() from the on-device UI are independent of this gate.
 */
void usb_mode_set_commands_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
