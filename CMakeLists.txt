# Top-level ESP-IDF project definition.
#
# `project.cmake` is what turns an ordinary CMake tree into an ESP-IDF build: it
# discovers `main/` and `components/`, wires in the framework's own components,
# and defines the `idf.py` targets. It has to be included *before* `project()`,
# because `project()` is what triggers that machinery.
#
# IDF_PATH comes from the environment, set by `. $IDF_PATH/export.sh`. Hard-coding
# a path here would tie the repository to one machine's layout.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32c3-pc-power)
