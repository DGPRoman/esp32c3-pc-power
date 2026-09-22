# power_machine.c is deliberately free of ESP-IDF: it is the part the host suite
# runs, and it stays runnable there only for as long as nothing in it needs the
# framework. power.c is the half that does, and it declares what it needs.
#
# REQUIRES rather than PRIV_REQUIRES for esp_common: power.h returns esp_err_t, so
# anything including it needs that header too.

idf_component_register(
    SRCS "power_machine.c" "power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_timer hw_gpio log freertos
)
