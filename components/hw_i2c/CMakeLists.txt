idf_component_register(
    SRCS "hw_i2c.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES soc hw_reg hw_gpio esp_rom
)
