/* Golden-vector generator for the LotofBench unit tests.
 *
 * The tests pin, as regression guards: the PRNG stream, the 1 MB seed-42
 * corpus (SHA-1 + head bytes), the LZ4 encoder output on fixed inputs
 * (length + SHA-1), and the default-seed edge-relaxation count. Those
 * pins live in tests/test_rng.c, tests/test_lz4.c,
 * tests/test_workload_compression.c and
 * tests/test_workload_navigation.c.
 *
 * After a DELIBERATE change to any pinned behaviour (never after an
 * accidental one), rebuild and run this tool and port the printed
 * values into the tests in the same commit:
 *
 *     make tools/gen_goldens && ./tools/gen_goldens
 */

#include "harness.h"
#include "lotofbench.h"
#include "lz4.h"
#include "rng.h"
#include "sha1.h"
#include "workload_compression.h"
#include "workload_html5browser.h"
#include "workload_navigation.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    char hex[41];

    /* rng stream, seed 42 */
    rng_t r;
    rng_init(&r, 42);
    printf("RNG42_FIRST3");
    for (int i = 0; i < 3; i++)
        printf(" %llu", (unsigned long long)rng_next(&r));
    rng_init(&r, 42);
    printf("\nRNG42_BELOW100_FIRST4");
    for (int i = 0; i < 4; i++)
        printf(" %u", rng_below(&r, 100));

    /* corpus (v0.1 compatibility pin), 1 MB seed 42 */
    fc_ctx *fc = fc_prepare(1, 42);
    sha1_hex(fc_corpus(fc), fc_len(fc), hex);
    printf("\nCORPUS_1MB_SEED42_SHA1 %s len=%zu", hex, fc_len(fc));
    printf("\nCORPUS_1MB_SEED42_HEAD");
    for (int i = 0; i < 8; i++)
        printf(" %02x", fc_corpus(fc)[i]);
    fc_free(fc);

    /* lz4 goldens: 4096 x 'a' */
    static uint8_t in[4096], out[16384];
    memset(in, 'a', sizeof in);
    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(in, sizeof in, out, level);
        sha1_hex(out, clen, hex);
        printf("\nLZ4_A4096_L%d clen=%zu sha1=%s", level, clen, hex);
    }

    /* lz4 goldens: 4096 rng bytes (seed 1234) */
    rng_init(&r, 1234);
    for (size_t i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)rng_next(&r);
    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(in, sizeof in, out, level);
        sha1_hex(out, clen, hex);
        printf("\nLZ4_RNG4096_L%d clen=%zu sha1=%s", level, clen, hex);
    }

    /* navigation default-seed edge count, single-threaded */
    nav_ctx *nav = nav_prepare(1);
    unsigned th = 1;
    double tp;
    uint64_t ops = 0;

    if (!nav || nav_run(nav, 1, &th, &tp, &ops) != 0)
        return 1;
    printf("\nNAV_SEED1_EDGES %llu threads_used=%u",
           (unsigned long long)ops, th);
    nav_free(nav);

    /* html5 goldens (seed 1): total DOM elements, page 0 (portal) and
     * page 5 (landing) sources and rendered pixmaps */
    html5_ctx *h5 = html5_prepare(1);
    static uint8_t pix[HTML5_VIEW_W * HTML5_VIEW_H * 4];
    static const unsigned HPAGES[] = { 0, 5 };

    if (!h5)
        return 1;
    printf("\nHTML5_SEED1_ELEMENTS %llu",
           (unsigned long long)html5_elements(h5));
    for (size_t i = 0; i < sizeof HPAGES / sizeof HPAGES[0]; i++) {
        unsigned p = HPAGES[i];
        uint64_t elts = html5_page_elements(h5, p);
        uint64_t recs = html5_page_records(h5, p);

        sha1_hex((const uint8_t *)html5_page_source(h5, p),
                 html5_page_source_len(h5, p), hex);
        printf("\nHTML5_SEED1_PAGE%u_SRC_SHA1 %s len=%zu elements=%llu "
               "records=%llu",
               p, hex, html5_page_source_len(h5, p),
               (unsigned long long)elts, (unsigned long long)recs);
        if (html5_render_page(h5, p, pix) != 0)
            return 1;
        sha1_hex(pix, sizeof pix, hex);
        printf("\nHTML5_SEED1_PAGE%u_PIX_SHA1 %s", p, hex);
    }
    html5_free(h5);
    return 0;
}
