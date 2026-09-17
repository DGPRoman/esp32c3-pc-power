/**
 * @file check.h
 * @brief The assertion harness these tests are written against.
 *
 * Written here rather than pulled in, for the same reason the drivers are: one
 * more dependency to install is one more reason not to run the tests. It is
 * deliberately small — a counter, a location, and enough printing that a failure
 * says what the value actually was.
 *
 * Nothing aborts on a failed check. A run reports every failure it found, because
 * the second failure is often what explains the first.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned check_checks;
extern unsigned check_failures;

void check_begin(const char *name);
void check_fail(const char *file, int line, const char *format, ...);
int check_summary(void);

/** @brief Fail unless @p expr holds. */
#define CHECK(expr)                                                  \
    do {                                                             \
        check_checks++;                                              \
        if (!(expr)) {                                               \
            check_fail(__FILE__, __LINE__, "expected %s", #expr);    \
        }                                                            \
    } while (0)

/** @brief Fail unless two integers are equal, printing both when they are not. */
#define CHECK_EQ(actual, expected)                                          \
    do {                                                                    \
        check_checks++;                                                     \
        const intmax_t check_a = (intmax_t)(actual);                        \
        const intmax_t check_e = (intmax_t)(expected);                      \
        if (check_a != check_e) {                                           \
            check_fail(__FILE__, __LINE__, "%s: expected %jd, got %jd",     \
                       #actual, check_e, check_a);                          \
        }                                                                   \
    } while (0)

/** @brief Fail unless two strings are equal, printing both when they are not. */
#define CHECK_EQ_STR(actual, expected)                                      \
    do {                                                                    \
        check_checks++;                                                     \
        const char *check_a = (actual);                                     \
        const char *check_e = (expected);                                   \
        if (check_a == NULL || strcmp(check_a, check_e) != 0) {             \
            check_fail(__FILE__, __LINE__, "%s: expected \"%s\", got \"%s\"", \
                       #actual, check_e, check_a == NULL ? "(null)" : check_a); \
        }                                                                   \
    } while (0)
