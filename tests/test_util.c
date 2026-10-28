/* LotofBench tests — failure reporting and the global failure counter. */

#include "tests.h"

#include <stdarg.h>
#include <stdio.h>

static int failures;

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    failures++;
    printf("  FAIL %s:%d: ", file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int test_failure_count(void)
{
    return failures;
}
