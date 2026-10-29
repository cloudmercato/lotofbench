#ifndef WORKLOAD_CLANG_H
#define WORKLOAD_CLANG_H

#include <stddef.h>
#include <stdint.h>

/* Workload #6 — Clang.
 *
 * Mirrors the sixth Geekbench 6 CPU workload: compiling source code with
 * a real toolchain (Geekbench 6 builds the Lua interpreter with Clang).
 * A system compiler cannot be part of a reproducible benchmark — machine
 *-dependent binaries, version-dependent codegen — so lotofbench
 * reproduces the shape of the compiler pipeline dependency-free, with
 * the same honesty rules as the other workloads: a hand-rolled lexer, a
 * recursive-descent parser and a bytecode code generator over
 * deterministic synthetic "translation units" written in a Lua-flavoured
 * subset. Everything is documented in docs/benchmarks.md, section
 * "Workload 6". */

typedef struct cl_ctx cl_ctx;

/* Fixed input size, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the sources are the sources. */
#define CLANG_UNITS 32u

/* Build the translation units (off the clock; fixed seed => byte-identical
 * sources on every machine) and the reference compilation of every one. */
cl_ctx *cl_prepare(unsigned seed);
void    cl_free(cl_ctx *ctx);

/* One workload pass with *threads threads (clamped to the unit count).
 * Fills *throughput_ktps with the median over the runs, in thousands of
 * source tokens compiled per second, and *tokens with the exact token
 * count of one pass (deterministic, identical for every run). Returns 0,
 * or -1 if a unit failed verification. */
int cl_run(cl_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_ktps, uint64_t *tokens);

/* Read-only access for tests and tooling. */
uint32_t    cl_units(const cl_ctx *ctx);
uint64_t    cl_tokens(const cl_ctx *ctx); /* source tokens per pass */
uint64_t    cl_unit_tokens(const cl_ctx *ctx, unsigned unit);
uint64_t    cl_unit_nodes(const cl_ctx *ctx, unsigned unit);
const char *cl_unit_source(const cl_ctx *ctx, unsigned unit);
size_t      cl_unit_source_len(const cl_ctx *ctx, unsigned unit);
const uint8_t *cl_unit_blob(const cl_ctx *ctx, unsigned unit, size_t *len);
uint64_t    cl_unit_digest(const cl_ctx *ctx, unsigned unit); /* SHA-1[0..7] */

/* Test hooks on arbitrary source. cl_lex_count lexes the chunk and
 * returns its token count, or -1 on a lexer error (unterminated string,
 * bad escape, bad character, numeric overflow). cl_parse_count lexes and
 * parses it and returns the AST node count, or -1 on any syntax error.
 * cl_compile runs the full pipeline and writes the serialized bytecode
 * blob into `out` (capacity `cap`), returning the blob length, or -1 on
 * any error or short capacity. All three use the context's tool scratch;
 * like the html5 hooks they are not safe to call from threads. */
long cl_lex_count(cl_ctx *ctx, const char *src, size_t len);
long cl_parse_count(cl_ctx *ctx, const char *src, size_t len);
long cl_compile(cl_ctx *ctx, const char *src, size_t len,
                uint8_t *out, size_t cap);

#endif /* WORKLOAD_CLANG_H */
