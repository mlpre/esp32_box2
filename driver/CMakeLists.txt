idf_component_register(
    SRCS
        "box2_audio.c"
        "box2_board.c"
        "box2_lcd.c"
        "box2_motion.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_adc
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_spi
        esp_lcd
)
