# REQUIRES rather than PRIV_REQUIRES: ssd1306.h returns hw_i2c_result_t, so the
# dependency is part of this component's public interface. Reporting the bus's own
# verdict is deliberate — "the display did not answer" and "the bus is stuck" call
# for different responses, and collapsing them into a bool would throw that away.

idf_component_register(
    SRCS "ssd1306.c"
    INCLUDE_DIRS "include"
    REQUIRES hw_i2c
)
