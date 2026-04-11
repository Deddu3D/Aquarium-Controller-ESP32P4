idf_component_register(
    SRCS "main.c" "wifi_manager.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer log
)
