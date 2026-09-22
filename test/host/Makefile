# Host tests. No ESP toolchain, no board, no network.
#
#     make          build and run
#     make clean
#
# Components are compiled for the host by pointing at test/host/stub for the four
# ESP-IDF headers they include. The stubs are deliberately thin: lwIP's socket API
# is the BSD one, so on a host it is enough to point at the real thing, and a
# component that starts using a framework symbol the stubs do not name will fail
# to build here rather than quietly go untested.
#
# Sanitisers are on by default. These tests feed the request parser truncated and
# oversized input on purpose, and an overread that the assertions happen not to
# notice is exactly what ASan is for.

CC ?= cc
COMPONENTS := ../../components

# -Wconversion is deliberately absent. Two of these suites include a component's
# translation unit, and that code is written for a 32-bit target where ssize_t is
# int; on a 64-bit host the same lines read as narrowing conversions that cannot
# happen where the firmware runs. The firmware's own warning set belongs to the
# target build, not here.
WARNINGS := -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
            -Wstrict-prototypes -Wmissing-prototypes -Wundef

SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS ?= -std=c11 -O1 -g
CPPFLAGS := -I. -Istub \
            $(patsubst %,-I%,$(wildcard $(COMPONENTS)/*/include))

TESTS := main.c check.c stub/platform.c \
         test_font5x7.c test_ssd1306.c test_http_request.c test_http_fields.c \
         test_hw_i2c_timing.c test_wifi_field.c

# Compiled and linked, rather than included by a test: their public API is enough.
SOURCES := $(COMPONENTS)/font5x7/font5x7.c \
           $(COMPONENTS)/http_fields/http_fields.c \
           $(COMPONENTS)/hw_i2c/hw_i2c_timing.c \
           $(COMPONENTS)/wifi_manager/wifi_field.c

# Included by a test rather than linked, because the code under test is static.
# Listed so that editing one rebuilds: without this a change to a component leaves
# the previous binary in place, and the suite reports on code that is no longer
# there — which is exactly how a mutation check gets a false clean.
INCLUDED := $(COMPONENTS)/ssd1306/ssd1306.c $(COMPONENTS)/http_server/http_server.c

HEADERS := $(wildcard check.h stub/*.h stub/*/*.h) \
           $(wildcard $(COMPONENTS)/*/include/*.h)

BINARY := host-tests

.PHONY: all run clean
all: run

$(BINARY): $(TESTS) $(SOURCES) $(INCLUDED) $(HEADERS)
	$(CC) $(CFLAGS) $(WARNINGS) $(SANITIZE) $(CPPFLAGS) -o $@ $(TESTS) $(SOURCES)

run: $(BINARY)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BINARY)

clean:
	rm -f $(BINARY)
