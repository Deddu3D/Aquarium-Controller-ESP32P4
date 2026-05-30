idf_component_register(
    SRCS "esp_lcd_hx8394.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_gpio freertos
)
