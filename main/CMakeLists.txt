idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer nvs_flash esp_netif esp_event
    PRIV_REQUIRES esp_hosted
)
