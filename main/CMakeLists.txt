idf_component_register(
    SRCS
        "main.c"
        "device_controls.c"
        "device_screen.c"
        "device_tone.c"
        "mic_stream.c"
        "usb_hid.c"
        "usb_mode.c"
        "usb_microphone.c"
        "voice_wake.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        nvs_flash
        esp_timer
        esp_ringbuf
        esp_driver_usb_serial_jtag
        esp_hal_usb
        usb_device_uac
        espressif__tinyusb
        espressif__esp-sr
)

# usb_device_uac 1.3.1 allocates feature-control state for speaker channels,
# even in microphone-only mode. Its stock descriptor advertises channel-1
# controls which can index past that state. Keep the stack and its task/PHY
# lifecycle, but replace just its descriptor with master-mute-only controls.
# The component adds this file PUBLIC to TinyUSB, including propagation to
# dependent targets. Remove it from both properties to avoid duplicate symbols.
idf_component_get_property(tinyusb_library espressif__tinyusb COMPONENT_LIB)
foreach(source_property SOURCES INTERFACE_SOURCES)
    get_target_property(tinyusb_sources ${tinyusb_library} ${source_property})
    set(stock_descriptor_count 0)
    foreach(tinyusb_source IN LISTS tinyusb_sources)
        if(tinyusb_source MATCHES "/tusb/usb_descriptors\\.c$")
            list(REMOVE_ITEM tinyusb_sources "${tinyusb_source}")
            math(EXPR stock_descriptor_count "${stock_descriptor_count} + 1")
        endif()
    endforeach()
    if(NOT stock_descriptor_count EQUAL 1)
        message(FATAL_ERROR "Expected one usb_device_uac 1.3.1 stock descriptor in ${source_property}")
    endif()
    set_property(TARGET ${tinyusb_library} PROPERTY ${source_property} "${tinyusb_sources}")
endforeach()
# Keep callbacks in the same archive as their TinyUSB consumers.
target_sources(${tinyusb_library} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c")

# The composite device adds a boot keyboard next to the UAC microphone.
# usb_device_uac supplies tusb_config.h, which does not enable HID, so set the
# class switch here. PUBLIC keeps usbd.c, hid_device.c and main/*.c consistent;
# a mismatch would compile the descriptor without the driver behind it.
target_compile_definitions(${tinyusb_library} PUBLIC CFG_TUD_HID=1)
