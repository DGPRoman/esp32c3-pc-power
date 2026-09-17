#include "check.h"

#include <stdarg.h>

unsigned check_checks;
unsigned check_failures;

static const char *s_current = "(no suite)";

void check_begin(const char *name)
{
    s_current = name;
}

void check_fail(const char *file, int line, const char *format, ...)
{
    check_failures++;

    (void)fprintf(stderr, "%s:%d: [%s] ", file, line, s_current);

    va_list arguments;
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);

    (void)fputc('\n', stderr);
}

int check_summary(void)
{
    if (check_failures == 0u) {
        (void)printf("%u checks passed\n", check_checks);
        return 0;
    }
    (void)printf("%u of %u checks FAILED\n", check_failures, check_checks);
    return 1;
}
