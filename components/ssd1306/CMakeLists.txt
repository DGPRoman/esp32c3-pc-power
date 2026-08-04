# Both dependencies are public rather than private, because ssd1306.h names things
# from each: it returns hw_i2c_result_t, and it derives its text grid from the font's
# metrics. Reporting the bus's own verdict is deliberate — "the display did not
# answer" and "the bus is stuck" call for different responses, and collapsing them
# into a bool would throw that distinction away.

idf_component_register(
    SRCS "ssd1306.c"
    INCLUDE_DIRS "include"
    REQUIRES hw_i2c font5x7
)
