idf_component_register(
    SRCS "wifi_store.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_hw_support
)
