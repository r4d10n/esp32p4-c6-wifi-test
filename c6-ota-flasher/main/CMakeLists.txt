idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer nvs_flash esp_event
    PRIV_REQUIRES esp_hosted
    EMBED_FILES "network_adapter.bin"
)
