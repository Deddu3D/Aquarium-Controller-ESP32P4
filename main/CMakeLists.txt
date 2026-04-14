idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c" "led_controller.c" "led_schedule.c"
         "led_scenes.c" "sun_position.c" "temperature_sensor.c" "temperature_history.c"
         "telegram_notify.c" "relay_controller.c" "duckdns.c" "ota_update.c"
         "auto_heater.c" "display_driver.c" "display_ui.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer log led_strip driver
             onewire_bus ds18b20 esp_http_client esp-tls app_update esp_app_format
             esp_lcd esp_hw_support esp_lcd_touch
    EMBED_TXTFILES "telegram_root_cert.pem"
)
