idf_component_register(
    SRCS "device_auth.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_hw_support
)
