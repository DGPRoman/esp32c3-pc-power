/**
 * @file main.c
 * @brief Runs every host suite and reports once.
 *
 * Suites are listed here by hand. A registration mechanism would be less typing
 * and more machinery than nine components warrant; a suite that is written and
 * not listed shows up as a linker warning about an unused function.
 */

#include "check.h"

void test_font5x7(void);
void test_ssd1306(void);
void test_http_request(void);

int main(void)
{
    test_font5x7();
    test_ssd1306();
    test_http_request();

    return check_summary();
}
