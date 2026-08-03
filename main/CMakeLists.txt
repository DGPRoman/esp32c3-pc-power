# The `main` component is special: ESP-IDF registers it last and links it against
# every other component, so it never has to declare dependencies on them. Our own
# drivers under `components/` do not get that privilege — each one declares what it
# needs via REQUIRES, which keeps the dependency graph honest and the build fast.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
