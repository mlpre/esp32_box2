idf_component_register(
    SRCS
        "main.c"
        "hardware_test_screen.c"
        "hardware_test_utils.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        spi_flash
)
