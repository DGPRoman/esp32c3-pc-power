# power_machine.c is deliberately free of ESP-IDF: it is the part the host suite
# runs, and it stays runnable there only for as long as nothing in it needs the
# framework. The hardware half lives beside it and declares what it needs.

idf_component_register(
    SRCS "power_machine.c"
    INCLUDE_DIRS "include"
)
