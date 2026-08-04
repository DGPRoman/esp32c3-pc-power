# PRIV_REQUIRES rather than REQUIRES: the register definitions from `soc` are used
# only inside hw_gpio.c, and hw_gpio.h exposes nothing from them. Declaring the
# dependency as private keeps it from leaking into every component that includes
# this driver's header.

idf_component_register(
    SRCS "hw_gpio.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES soc
)
