idf_component_register(
    SRCS
        "rgb_stream_main.c"
        "hardware_test_screen.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
        spi_flash
)
