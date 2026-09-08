if(BOX2_HARDWARE_TEST)
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
else()
idf_component_register(
    SRCS "game_main.c" "game.c" "game_render.c" "game_selftest.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer nvs_flash
    EMBED_FILES "assets/bgm.pcm" "assets/voice.pcm"
)
endif()
