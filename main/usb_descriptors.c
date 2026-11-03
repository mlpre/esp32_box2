/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2020 Jerzy Kasenbreg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "tusb.h"
#include "class/hid/hid_device.h"
#include "uac_descriptors.h"

// usb_device_uac 1.3.1 sizes its feature-control arrays from speaker channels.
// With a microphone-only device they contain only master channel 0, so the
// upstream descriptor must not advertise writable controls on microphone CH1.
// Expose master mute only: the application applies it to captured PCM, while
// microphone gain remains under the device's physical-button control.
#undef MIC_CTRL
#define MIC_CTRL U32_TO_U8S_LE(AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS), U32_TO_U8S_LE(0)

// Mirror the upstream MIC-only descriptor, retaining its layout and lengths.
// Its input terminal advertised a readable connector control, but the UAC
// driver's entity request handler implements only clock and feature units.
// Do not advertise that unsupported input-terminal control to the host.
#define BOX2_AUDIO_MIC_DESCRIPTOR(_itfnum, _stridx, _epin) \
    TUD_AUDIO_DESC_IAD(_itfnum, NUM_INTERFACES, 0x00), \
    TUD_AUDIO_DESC_STD_AC(_itfnum, 0x00, _stridx), \
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_MICROPHONE, TUD_AUDIO_DESC_CS_AC_TOTAL_LEN, AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS), \
    TUD_AUDIO_DESC_CLK_SRC(UAC2_ENTITY_CLOCK, 1, 1, UAC2_ENTITY_MIC_INPUT_TERMINAL, 0x00), \
    TUD_AUDIO_DESC_INPUT_TERM(UAC2_ENTITY_MIC_INPUT_TERMINAL, AUDIO_TERM_TYPE_IN_GENERIC_MIC, UAC2_ENTITY_MIC_OUTPUT_TERMINAL, \
                             UAC2_ENTITY_CLOCK, MIC_CHANNEL_NUM, AUDIO_CHANNEL_CONFIG_FRONT_CENTER, 0x00, AUDIO_CTRL_NONE, 0x00), \
    TUD_AUDIO_DESC_OUTPUT_TERM(UAC2_ENTITY_MIC_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, UAC2_ENTITY_MIC_INPUT_TERMINAL, \
                              UAC2_ENTITY_MIC_FEATURE_TERMINAL, UAC2_ENTITY_CLOCK, 0x0000, 0x00), \
    TUD_AUDIO_DESC_FEATURE_UNIT_N_CHANNEL(TUD_AUDIO_DESC_MIC_FEATURE_UNIT_N_CHANNEL_LEN, UAC2_ENTITY_MIC_FEATURE_TERMINAL, \
                                         UAC2_ENTITY_MIC_INPUT_TERMINAL, 0x00, MIC_CTRL), \
    TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 1, 0x00, 0x00, _stridx + 1), \
    TUD_AUDIO_DESC_STD_AS_INT(_itfnum + 1, 0x01, 0x01, _stridx + 1), \
    TUD_AUDIO_DESC_CS_AS_INT(UAC2_ENTITY_MIC_OUTPUT_TERMINAL, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, AUDIO_DATA_FORMAT_TYPE_I_PCM, \
                           MIC_CHANNEL_NUM, AUDIO_CHANNEL_CONFIG_FRONT_CENTER, 0x00), \
    TUD_AUDIO_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX), \
    TUD_AUDIO_DESC_STD_AS_ISO_EP(_epin, TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA, \
                               CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN, 1), \
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE, \
                              AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000)

#undef TUD_AUDIO_DESCRIPTOR
#define TUD_AUDIO_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epfb) BOX2_AUDIO_MIC_DESCRIPTOR(_itfnum, _stridx, _epin)

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // Use Interface Association Descriptor (IAD) for CDC
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = CONFIG_UAC_TUSB_VID,
    .idProduct          = CONFIG_UAC_TUSB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

// A boot keyboard sits next to the microphone so the device can start dictation
// in the host application instead of waiting for a mouse click. Interface
// numbers follow the audio function, which owns ITF_NUM_TOTAL interfaces.
#define ITF_NUM_HID            ITF_NUM_TOTAL
#define ITF_NUM_ALL            (ITF_NUM_TOTAL + 1)

#define CONFIG_TOTAL_LEN        (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO_DEVICE_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_AUDIO_OUT   0x01
#define EPNUM_AUDIO_FB    0x81
#define EPNUM_AUDIO_IN    0x82
#define EPNUM_HID_IN      0x83

// Boot keyboard with a 1-byte LED output report. The output byte is unused for
// now but keeps a host-to-device channel available without a driver.
static uint8_t const desc_hid_report[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute) - modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant) - reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant) - LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) - key array
    0xC0               // End Collection
};

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_ALL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4, EPNUM_AUDIO_OUT, EPNUM_AUDIO_IN, EPNUM_AUDIO_FB),
    // Interface number, string index (0 = none), protocol, report length, EP In, size, polling ms
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report),
                       EPNUM_HID_IN, 8, 5),
};

//--------------------------------------------------------------------+
// HID callbacks
//--------------------------------------------------------------------+

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    // Only the keyboard report is defined, and the host never reads it back.
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    // LED state from the host. Unused today; kept so the interface stays a plain
    // boot keyboard and Windows installs no extra driver.
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}


// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // for multiple configurations
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const *string_desc_arr [] = {
    (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
    CONFIG_UAC_TUSB_MANUFACTURER,       // 1: Manufacturer
    CONFIG_UAC_TUSB_PRODUCT,            // 2: Product
    CONFIG_UAC_TUSB_SERIAL_NUM,         // 3: Serials, should use chip ID
    "usb uac",                      // 4: UAC control Interface
#if SPEAK_CHANNEL_NUM
    "speaker",                     // 5: Speak Interface
#endif
#if MIC_CHANNEL_NUM
    "microphone",                   // 6: Mic Interface
#endif
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        const char *str = string_desc_arr[index];

        // Cap at max char
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
