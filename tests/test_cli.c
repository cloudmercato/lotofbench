/* LotofBench tests — the strict CLI number parser (cli.[ch]). */

#include "tests.h"

#include "cli.h"

void test_parse_uint_valid(void)
{
    unsigned v;

    CHECK(lb_parse_uint("0", &v) == 0);
    CHECK_U64(v, 0);
    CHECK(lb_parse_uint("12", &v) == 0);
    CHECK_U64(v, 12);
    CHECK(lb_parse_uint("00", &v) == 0);
    CHECK_U64(v, 0);
    CHECK(lb_parse_uint("00012", &v) == 0);
    CHECK_U64(v, 12);
    CHECK(lb_parse_uint("4294967295", &v) == 0); /* UINT_MAX */
    CHECK_U64(v, 4294967295u);
}

void test_parse_uint_invalid(void)
{
    unsigned v = 42;

    /* Every shape strtoul would silently mis-parse must be rejected
     * with the value left untouched. */
    const char *bad[] = { "", "abc", "12abc", "abc12", "-4", "+4", " 4",
                          "4 ", "1.5", "--", "99999999999999999999" };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(lb_parse_uint(bad[i], &v) == -1);
    CHECK_U64(v, 42); /* untouched on every rejection */
}
