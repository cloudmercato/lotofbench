#ifndef ITRIG_H
#define ITRIG_H

#include <stdint.h>

/* Shared integer trigonometry table: cos/sin of the 720 bin-centre
 * angles (0.25-degree bins), scaled by 1024, rounded half away from
 * zero at generation time — bit-identical on every machine without
 * calling libm. Out-of-range bins give 0. See docs/benchmarks.md. */
#define ITRIG_BINS 720

void itrig_trig(unsigned theta_bin, int32_t *cos1024, int32_t *sin1024);

/* Direct table access for hot loops (each returns ITRIG_BINS entries,
 * x1024 scale, bin k = centre of [k * 0.25, (k + 1) * 0.25) degrees). */
const int16_t *itrig_cos(void);
const int16_t *itrig_sin(void);

#endif /* ITRIG_H */
