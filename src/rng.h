#ifndef RNG_H
#define RNG_H

#include <stdint.h>

/* Deterministic PRNG stream (xorshift*, the algorithm LotofBench has used
 * since v0.1). Same seed => same sequence on every machine, which is what
 * makes the synthetic inputs byte-identical everywhere. Each workload
 * owns its own stream so the inputs are independent of each other. */

typedef struct {
    uint64_t state;
} rng_t;

void     rng_init(rng_t *r, uint64_t seed);
uint64_t rng_next(rng_t *r);
uint32_t rng_below(rng_t *r, uint32_t n);

#endif /* RNG_H */
