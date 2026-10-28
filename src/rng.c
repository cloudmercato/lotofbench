/* LotofBench — deterministic PRNG stream (xorshift*). */

#include "rng.h"

void rng_init(rng_t *r, uint64_t seed)
{
    r->state = seed * 0x9E3779B97F4A7C15ull + 0xA0761D6478BD642Full;
    rng_next(r); /* the first output is correlated with the seed, drop it */
}

uint64_t rng_next(rng_t *r)
{
    r->state ^= r->state >> 12;
    r->state ^= r->state << 25;
    r->state ^= r->state >> 27;
    return r->state * 2685821657736338717ull;
}

uint32_t rng_below(rng_t *r, uint32_t n)
{
    return (uint32_t)(rng_next(r) % n);
}
