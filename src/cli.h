#ifndef CLI_H
#define CLI_H

#include <stdio.h>

/* LotofBench — suite driver command line (parsed for main.c). */

typedef struct {
    unsigned corpus_mb, runs, seed, threads; /* requested thread count */
    int json;
    const char *json_out; /* also store the JSON document in this file */
    int upload; /* submit the document to the portal (see parse_args) */
} options;

/* Strict unsigned decimal parse of the whole string: digits only, no
 * sign, no leading/trailing junk, no overflow. Returns 0 on success and
 * stores the value through `out`; returns -1 otherwise (leaving `out`
 * untouched). */
int lb_parse_uint(const char *s, unsigned *out);

/* Parse the driver's argv into options. Exits with status 2 on a bad or
 * incomplete flag or an invalid value, and with status 0 on --help.
 * Upload defaults to ON when the binary was built with libcurl and to
 * OFF on the zero-dependency build: --no-upload short-circuits every
 * network call (zero sockets), --upload is refused with status 2 on a
 * build that has no curl support. */
options parse_args(int argc, char **argv);

#endif /* CLI_H */
