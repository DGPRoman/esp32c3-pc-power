#!/bin/sh
# Static analysis over every C file in this repository.
#
# The firmware build already fails on any compiler warning, which catches a great
# deal; what it does not do is follow a value across branches to find the path where
# a buffer is indexed out of range or a variable is read before it is set. cppcheck
# does, and it does it on code that never runs — which for a board that needs a
# radio, a panel and a PC to exercise properly is most of it.
#
# Run it the same way CI does:
#
#     sh test/check-static.sh
#
# POSIX shell, like the contract check: a check that needed a toolchain this
# repository does not otherwise have is a check that gets skipped.

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "check-static: cppcheck is not installed" >&2
    echo "  Debian/Ubuntu: apt-get install cppcheck" >&2
    exit 127
fi

# Every component's public headers, discovered rather than listed, so a component
# added tomorrow is analysed without anyone remembering to come back here.
includes=""
for dir in "$root"/components/*/include; do
    [ -d "$dir" ] || continue
    includes="$includes -I$dir"
done

# MACSTR and MAC2STR come from esp_mac.h, which is not on this include path. Left
# undefined, cppcheck abandons the configuration containing them and reports the
# macro rather than the file — so they are supplied here exactly as the framework
# defines them, which keeps the format string and its arguments checkable.
#
# --check-level=exhaustive: the default gives up on branch-heavy functions, and
# on_http_request and the SSD1306 init sequence are exactly that shape.
#
# --inline-suppr: a finding that is deliberate is answered next to the line, with
# the reason, rather than in a list here that nobody reads beside the code.
#
# The two missingInclude suppressions are about cppcheck's view of the tree, not
# about this code: the framework headers are genuinely absent outside the IDF
# container, and saying so once is better than a page of it.
# shellcheck disable=SC2086  # $includes is a list of flags and must word-split.
cppcheck \
    --enable=warning,style,performance,portability \
    --std=c11 --language=c --platform=unix32 \
    --check-level=exhaustive \
    --inline-suppr \
    --error-exitcode=1 \
    --quiet \
    -DMACSTR='"%02x:%02x:%02x:%02x:%02x:%02x"' \
    -D'MAC2STR(a)=(a)[0],(a)[1],(a)[2],(a)[3],(a)[4],(a)[5]' \
    $includes \
    --suppress=missingInclude \
    --suppress=missingIncludeSystem \
    --suppress=normalCheckLevelMaxBranches \
    "$root/components" "$root/main"

echo "check-static: cppcheck found nothing."
