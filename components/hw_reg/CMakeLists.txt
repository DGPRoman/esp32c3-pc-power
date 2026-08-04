# Header-only: no SRCS, so this registers as an interface library and contributes
# nothing but its include path to whoever requires it.

idf_component_register(INCLUDE_DIRS "include")
