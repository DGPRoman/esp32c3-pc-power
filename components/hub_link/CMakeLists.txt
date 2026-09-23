# hub_announce.c is deliberately free of ESP-IDF: it is the part the host suite runs,
# and it stays runnable there only for as long as nothing in it needs the framework.

idf_component_register(
    SRCS "hub_announce.c"
    INCLUDE_DIRS "include"
)
