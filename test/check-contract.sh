#!/bin/sh
# The routes the firmware answers and the routes the contract documents are the
# same set, or this fails.
#
# A contract document is worth exactly as much as its agreement with the code,
# and the usual way that agreement ends is a route added in a hurry and written
# down later — which is to say, never. This is the cheapest check that notices.
#
# It compares route *paths*, not methods or fields. Those are prose and no script
# is going to read them; what it can do is make sure nothing is served in silence
# and nothing is promised that does not exist.
#
# POSIX shell and nothing else, like the commit hook: a check that needed a
# toolchain this repository does not otherwise have is a check that gets skipped.

set -eu

# CDPATH emptied rather than trusted: a value in the environment makes cd print
# where it went and land somewhere else, which would silently check the wrong tree.
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
source=$root/main/main.c
doc=$root/docs/http-api.md

for file in "$source" "$doc"; do
    if [ ! -f "$file" ]; then
        echo "check-contract: no such file: $file" >&2
        exit 1
    fi
done

# Every path the router compares against. The single place routing is decided is
# on_http_request, and it decides it with strcmp against a literal.
served=$(sed -n 's/.*strcmp(request->target, "\([^"]*\)").*/\1/p' "$source" | sort -u)

# Every path the document names with a method, which is how it names all of them:
# `GET /v1/power`, `POST /network`. A path mentioned in prose without a method is
# a reference rather than a definition and does not count.
# shellcheck disable=SC2016  # The backticks are Markdown's, not a substitution.
documented=$(
    grep -o '`\(GET\|HEAD\|POST\|PUT\|PATCH\|DELETE\) /[^`]*`' "$doc" |
        sed 's/^`[A-Z]* //; s/`$//' |
        sort -u
)

if [ -z "$served" ]; then
    echo "check-contract: found no routes in $source — has the router changed shape?" >&2
    exit 1
fi

# comm(1) takes files, and POSIX sh has no process substitution to fake them with.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

printf '%s\n' "$served" > "$work/served"
printf '%s\n' "$documented" > "$work/documented"

undocumented=$(comm -23 "$work/served" "$work/documented")
unserved=$(comm -13 "$work/served" "$work/documented")

status=0

if [ -n "$undocumented" ]; then
    echo "Served by main/main.c and absent from docs/http-api.md:" >&2
    echo "$undocumented" | sed 's/^/  /' >&2
    status=1
fi

if [ -n "$unserved" ]; then
    echo "Documented in docs/http-api.md and not served by main/main.c:" >&2
    echo "$unserved" | sed 's/^/  /' >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "check-contract: $(echo "$served" | wc -l | tr -d ' ') routes, documented and served."
fi

exit "$status"
