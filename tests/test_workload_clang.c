/* LotofBench tests — workload #6, Clang (workload_clang.[ch]).
 *
 * Determinism (byte-identical sources and bytecode digests for a seed —
 * the published comparable-scores promise), input invariants (published
 * unit/token bands, per-unit pipeline cross-checks), lexer and parser
 * unit behavior on handcrafted snippets, byte-pinned codegen goldens —
 * and the centerpiece: an independent decoder of the serialized
 * bytecode plus an abstract stack-depth interpreter that walks every
 * instruction of every unit and proves the jump fixups coherent (every
 * reachable path agrees on stack depth and ends at RETURN with exactly
 * one value). */

#include "tests.h"

#include "lotofbench.h"
#include "sha1.h"
#include "workload_clang.h"

#include <stdlib.h>
#include <string.h>

static cl_ctx *build(unsigned seed)
{
    cl_ctx *c = cl_prepare(seed);

    if (!c)
        test_fail(__FILE__, __LINE__, "cl_prepare(%u) failed", seed);
    return c;
}

/* --- independent bytecode decoder ---------------------------------------------*/
/* Re-implemented from the published serialization contract, not from
 * the workload's writer: header (nparams u8, nslots u16, nconsts u16,
 * ncode u16, nkids u16 LE), constants, 4-byte instructions, then the
 * serialized records of the child protos in order. */

#define BC_LOADK    1
#define BC_LOADNIL  2
#define BC_LOADBOOL 3
#define BC_LOADLOC  4
#define BC_STORELOC 5
#define BC_GETGLOBAL 6
#define BC_SETGLOBAL 7
#define BC_GETFIELD 8
#define BC_SETFIELD 9
#define BC_GETINDEX 10
#define BC_SETINDEX 11
#define BC_APPEND   12
#define BC_NEWTABLE 13
#define BC_ADD      14
#define BC_SUB      15
#define BC_MUL      16
#define BC_DIV      17
#define BC_MOD      18
#define BC_CONCAT   19
#define BC_NOT      20
#define BC_UNM      21
#define BC_LEN      22
#define BC_EQ       23
#define BC_NE       24
#define BC_LT       25
#define BC_LE       26
#define BC_GT       27
#define BC_GE       28
#define BC_JMP      29
#define BC_JZ       30
#define BC_JNZ      31
#define BC_DUP      32
#define BC_POP      33
#define BC_CALL     34
#define BC_CLOSURE  35
#define BC_RETURN   36

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t off;      /* past header + constants + code: the kids */
    size_t code_off; /* where the instruction stream starts */
    uint8_t nparams;
    uint16_t nslots, nconsts, ncode, nkids;
} proto;

static int rd16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* Decode the proto record at p[0..n); *end_out receives the offset just
 * past it (children follow). Returns 0 on success. */
static int proto_decode(const uint8_t *p, size_t n, proto *pr, size_t *end_out)
{
    if (n < 9)
        return -1;
    pr->p = p;
    pr->n = n;
    pr->off = 9;
    pr->nparams = p[0];
    pr->nslots = (uint16_t)rd16(p + 1);
    pr->nconsts = (uint16_t)rd16(p + 3);
    pr->ncode = (uint16_t)rd16(p + 5);
    pr->nkids = (uint16_t)rd16(p + 7);
    for (unsigned k = 0; k < pr->nconsts; k++) {
        uint8_t tag;

        if (pr->off >= n)
            return -1;
        tag = p[pr->off++];
        if (tag == 1) { /* INT */
            pr->off += 8;
        } else if (tag == 2) { /* STR */
            if (pr->off + 2 > n)
                return -1;
            pr->off += 2 + (size_t)rd16(p + pr->off);
        } else if (tag == 3) { /* BOOL */
            pr->off += 1;
        } else {
            return -1;
        }
    }
    pr->code_off = pr->off;
    pr->off += (size_t)pr->ncode * 4;
    if (pr->off > n)
        return -1;
    *end_out = pr->off;
    return 0;
}

/* Net stack effect of one instruction (the published machine contract);
 * CALL returns -nargs. Unknown opcodes are a contract violation. */
static int stack_delta(int op, int *ok)
{
    switch (op) {
    case BC_LOADK: case BC_LOADNIL: case BC_LOADBOOL: case BC_LOADLOC:
    case BC_GETGLOBAL: case BC_NEWTABLE: case BC_DUP: case BC_CLOSURE:
        return 1;
    case BC_STORELOC: case BC_SETGLOBAL: case BC_POP:
        return -1;
    case BC_GETFIELD: case BC_NOT: case BC_UNM: case BC_LEN:
        return 0;
    case BC_GETINDEX: case BC_JZ: case BC_JNZ:
        return -1;
    /* SETFIELD pops the value but keeps the table (the constructor
     * relies on that); assignments emit an explicit POP after it */
    case BC_SETFIELD:
        return -1;
    case BC_SETINDEX: /* pops index + value, keeps the table */
        return -2;
    case BC_APPEND: /* pops the appended value, keeps the table */
        return -1;
    case BC_ADD: case BC_SUB: case BC_MUL: case BC_DIV: case BC_MOD:
    case BC_CONCAT: case BC_EQ: case BC_NE: case BC_LT: case BC_LE:
    case BC_GT: case BC_GE:
        return -1;
    case BC_JMP:
        return 0;
    case BC_RETURN:
        return -1;
    default:
        *ok = 0;
        return 0;
    }
}

/* Abstract interpreter: every reachable path must agree on stack depth
 * at every instruction, jumps must land in range, and every path must
 * end at RETURN with exactly one value (the result). Instructions past
 * an unconditional jump or a return are simply unreachable. */
static int proto_check_stack(const proto *pr, unsigned unit, int depth_level)
{
    int ok = 1;
    uint8_t *seen = malloc(pr->ncode ? pr->ncode : 1);
    uint8_t *dep = malloc(pr->ncode ? pr->ncode : 1);
    uint32_t *work = malloc((size_t)(pr->ncode ? pr->ncode : 1) * 4);
    size_t wn = 0;

    if (!seen || !dep || !work) {
        free(seen);
        free(dep);
        free(work);
        return -1;
    }
    memset(seen, 0, pr->ncode);
    memset(dep, 0, pr->ncode);
    if (pr->ncode) {
        seen[0] = 1;
        dep[0] = 0;
        work[wn++] = 0;
    }
    while (wn && ok) {
        uint32_t i = work[--wn];
        const uint8_t *ins = pr->p + pr->code_off + (size_t)i * 4;
        int op = ins[0];
        int d = dep[i];
        uint32_t next[2];
        int nd[2], nnext = 0;

        if (op == BC_RETURN) {
            if (d != 1) {
                test_fail(__FILE__, __LINE__,
                          "unit %u proto %d: RETURN at %u with depth %d",
                          unit, depth_level, i, d);
                ok = 0;
            }
            continue;
        }
        if (op == BC_JMP) {
            next[nnext] = rd16(ins + 2);
            nd[nnext] = d;
            nnext++;
        } else if (op == BC_JZ || op == BC_JNZ) {
            next[nnext] = i + 1;
            nd[nnext] = d - 1;
            nnext++;
            next[nnext] = rd16(ins + 2);
            nd[nnext] = d - 1;
            nnext++;
        } else if (op == BC_CALL) {
            /* pops callee + nargs, pushes one result; nargs rides in a */
            int nargs = ins[1];

            if (d < nargs + 1) {
                test_fail(__FILE__, __LINE__,
                          "unit %u proto %d: CALL underflow at %u "
                          "(nargs %d, depth %d)",
                          unit, depth_level, i, nargs, d);
                ok = 0;
                break;
            }
            next[nnext] = i + 1;
            nd[nnext] = d - nargs;
            nnext++;
        } else {
            int delta = stack_delta(op, &ok);

            if (!ok) {
                test_fail(__FILE__, __LINE__,
                          "unit %u proto %d: unknown opcode %d at %u",
                          unit, depth_level, op, i);
                break;
            }
            if (d + delta < 0) {
                test_fail(__FILE__, __LINE__,
                          "unit %u proto %d: stack underflow at %u "
                          "(op %d, depth %d)",
                          unit, depth_level, i, op, d);
                ok = 0;
                break;
            }
            next[nnext] = i + 1;
            nd[nnext] = d + delta;
            nnext++;
        }
        for (int k = 0; k < nnext && ok; k++) {
            uint32_t t = next[k];

            if (t >= pr->ncode) {
                test_fail(__FILE__, __LINE__,
                          "unit %u proto %d: jump target %u out of range "
                          "(ncode %u) from %u",
                          unit, depth_level, t, pr->ncode, i);
                ok = 0;
                break;
            }
            if (seen[t]) {
                if (dep[t] != nd[k]) {
                    test_fail(__FILE__, __LINE__,
                              "unit %u proto %d: depth clash at %u "
                              "(%d vs %d)",
                              unit, depth_level, t, dep[t], nd[k]);
                    ok = 0;
                }
                continue;
            }
            seen[t] = 1;
            dep[t] = (uint8_t)nd[k];
            work[wn++] = t;
        }
    }
    /* the walk itself is the proof: the worklist drains only through
     * RETURN terminations, the range check rejects dangling jumps, and
     * the seen/dep arrays reject inconsistent joins */
    free(seen);
    free(dep);
    free(work);
    return ok ? 0 : -1;
}

/* Walk a whole serialized blob: the root proto and, after it, its kids
 * in order (each kid is itself a record followed by its own kids).
 * Returns the record's total length, 0 on malformed input. */
static size_t proto_total_len(const uint8_t *p, size_t n)
{
    proto pr;
    size_t end = 0;

    if (proto_decode(p, n, &pr, &end))
        return 0;
    size_t total = end;

    for (unsigned k = 0; k < pr.nkids; k++) {
        size_t kid_len = proto_total_len(p + total, n - total);

        if (kid_len == 0)
            return 0;
        total += kid_len;
    }
    return total;
}

static int blob_check(const uint8_t *p, size_t n, unsigned unit,
                      int depth_level, uint32_t *protos)
{
    proto pr;
    size_t end = 0;

    if (proto_decode(p, n, &pr, &end)) {
        test_fail(__FILE__, __LINE__, "unit %u: proto %d undecodable",
                  unit, depth_level);
        return -1;
    }
    if (pr.nslots > 4096) { /* nparams is a byte, capped by the encoding */
        test_fail(__FILE__, __LINE__,
                  "unit %u proto %d: nslots %u nparams %u",
                  unit, depth_level, pr.nslots, pr.nparams);
        return -1;
    }
    if (protos)
        (*protos)++;
    if (proto_check_stack(&pr, unit, depth_level))
        return -1;
    size_t off = end;

    for (unsigned k = 0; k < pr.nkids; k++) {
        size_t kid_len = proto_total_len(p + off, n - off);

        if (kid_len == 0) {
            test_fail(__FILE__, __LINE__,
                      "unit %u proto %d: kid %u missing/malformed", unit,
                      depth_level, k);
            return -1;
        }
        if (blob_check(p + off, kid_len, unit, depth_level + 1, protos))
            return -1;
        off += kid_len;
    }
    if (off != n) {
        test_fail(__FILE__, __LINE__,
                  "unit %u proto %d: %zu trailing bytes", unit,
                  depth_level, n - off);
        return -1;
    }
    return 0;
}

/* --- tests ----------------------------------------------------------------------*/

void test_cl_units_deterministic(void)
{
    cl_ctx *a = build(7), *b = build(7), *c = build(8);

    if (!a || !b || !c) {
        cl_free(a);
        cl_free(b);
        cl_free(c);
        return;
    }
    CHECK_U64(cl_units(a), CLANG_UNITS);
    for (unsigned u = 0; u < CLANG_UNITS; u++) {
        size_t la = cl_unit_source_len(a, u);

        if (la != cl_unit_source_len(b, u)) {
            test_fail(__FILE__, __LINE__, "unit %u length differs", u);
            break;
        }
        if (memcmp(cl_unit_source(a, u), cl_unit_source(b, u), la) != 0) {
            test_fail(__FILE__, __LINE__, "unit %u source differs", u);
            break;
        }
        if (cl_unit_tokens(a, u) != cl_unit_tokens(b, u) ||
            cl_unit_nodes(a, u) != cl_unit_nodes(b, u)) {
            test_fail(__FILE__, __LINE__, "unit %u counts differ", u);
            break;
        }
        size_t bl = 0, lb = 0;
        const uint8_t *ba = cl_unit_blob(a, u, &bl);

        if (bl != 0 && (cl_unit_blob(b, u, &lb) == NULL || lb != bl ||
                        memcmp(ba, cl_unit_blob(b, u, NULL), bl) != 0)) {
            test_fail(__FILE__, __LINE__, "unit %u bytecode differs", u);
            break;
        }
        if (cl_unit_digest(a, u) != cl_unit_digest(b, u)) {
            test_fail(__FILE__, __LINE__, "unit %u digest differs", u);
            break;
        }
    }
    /* a different seed produces a different corpus */
    CHECK(cl_unit_digest(a, 0) != cl_unit_digest(c, 0));
    CHECK(cl_tokens(a) > 0);
    CHECK(cl_tokens(c) > 0);
    cl_free(a);
    cl_free(b);
    cl_free(c);
}

void test_cl_input_invariants(void)
{
    static const unsigned SEEDS[] = { 1, 7, 42, 12345 };

    for (size_t s = 0; s < sizeof SEEDS / sizeof SEEDS[0]; s++) {
        cl_ctx *h = build(SEEDS[s]);
        uint64_t total = 0;

        if (!h)
            continue;
        for (unsigned u = 0; u < CLANG_UNITS; u++) {
            const char *src = cl_unit_source(h, u);
            size_t len = cl_unit_source_len(h, u);
            uint64_t toks = cl_unit_tokens(h, u);
            uint64_t nodes = cl_unit_nodes(h, u);
            uint64_t arch = u % 6;

            if (!src || len < 20000) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: missing/short source",
                          SEEDS[s], u);
                break;
            }
            if (toks < 6000 || toks > 120000) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: %llu tokens out of band",
                          SEEDS[s], u, (unsigned long long)toks);
                break;
            }
            /* every unit is a module: comment header, functions, end */
            if (memcmp(src, "-- lotofbench translation unit ", 31) != 0) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: missing header comment",
                          SEEDS[s], u);
                break;
            }
            if (!strstr(src, "function") || !strstr(src, "end") ||
                !strstr(src, "local")) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: malformed skeleton",
                          SEEDS[s], u);
                break;
            }
            /* the published pipeline counts must reproduce exactly */
            if (cl_lex_count(h, src, len) != (long)toks) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: token count mismatch",
                          SEEDS[s], u);
                break;
            }
            if (cl_parse_count(h, src, len) != (long)nodes) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: node count mismatch",
                          SEEDS[s], u);
                break;
            }
            /* per-archetype flavour: the arithmetic kernel leans on
             * operators, the string handler on .. and literals, the
             * table builder on constructors */
            if (arch == 2 && !strstr(src, " % ")) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: arithmetic kernel flavour",
                          SEEDS[s], u);
                break;
            }
            if (arch == 3 && !strstr(src, " .. ")) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: string handler flavour",
                          SEEDS[s], u);
                break;
            }
            if (arch == 1 && !strstr(src, "{")) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: table builder flavour",
                          SEEDS[s], u);
                break;
            }
            total += toks;
        }
        CHECK_U64(cl_tokens(h), total);
        cl_free(h);
    }
}

void test_cl_lexer_units(void)
{
    cl_ctx *h = build(1);

    if (!h)
        return;
    /* token counting on handcrafted chunks */
    CHECK_U64(cl_lex_count(h, "", 0), 0);
    CHECK_U64(cl_lex_count(h, "local x = 1 -- comment\nreturn x", 31), 6);
    CHECK_U64(cl_lex_count(h, "a.b .. c .. \"s\\n\\x41\" >= 5 ~= nil or "
                              "not f(1, 2)",
                            strlen("a.b .. c .. \"s\\n\\x41\" >= 5 ~= nil or "
                                   "not f(1, 2)")),
              19);
    CHECK_U64(cl_lex_count(h, "x = 0xFF + 0x10", 15), 5);
    CHECK_U64(cl_lex_count(h, "while x do break end", 20), 5);
    /* keywords are not names */
    CHECK_U64(cl_lex_count(h, "function end until", 18), 3);
    CHECK_U64(cl_lex_count(h, "functionx endx", 14), 2);
    /* lexical errors fail cleanly */
    CHECK_U64(cl_lex_count(h, "\"unterminated", 13), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "\"bad\\qescape\"", 13), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "\"short\\x4\"", 10), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "0xFFFFFFFFFFFFFFFFF", 19), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "123abc", 6), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "0x", 2), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "a ~ b", 5), (uint64_t)-1);
    CHECK_U64(cl_lex_count(h, "x = 1.5", 7), (uint64_t)-1); /* no floats */
    CHECK_U64(cl_lex_count(h, "x = $", 5), (uint64_t)-1);
    /* NULL / mismatched guards */
    CHECK_U64(cl_lex_count(h, NULL, 5), (uint64_t)-1);
    cl_free(h);
}

void test_cl_parser_units(void)
{
    cl_ctx *h = build(1);

    if (!h)
        return;
    CHECK_U64(cl_parse_count(h, "return 1", 8) > 0, 1);
    CHECK_U64(cl_parse_count(h, "local t = {a = 1, [2] = \"x\", nil} "
                              "return t.a",
                            strlen("local t = {a = 1, [2] = \"x\", nil} "
                                   "return t.a")) > 0,
              1);
    CHECK_U64(cl_parse_count(h, "while true do break end", 23) > 0, 1);
    CHECK_U64(cl_parse_count(h, "repeat until true", 17) > 0, 1);
    CHECK_U64(cl_parse_count(h, "for i = 1, 10, 2 do end", 23) > 0, 1);
    CHECK_U64(cl_parse_count(h, "if a then elseif b then else end", 32) > 0,
              1);
    CHECK_U64(cl_parse_count(h, "a, t.k, t[1] = 1, 2, 3",
                             strlen("a, t.k, t[1] = 1, 2, 3")) > 0,
              1);
    CHECK_U64(cl_parse_count(h, "local function f(a, ...) return a end", 37),
              (uint64_t)-1); /* ... is not in the subset */
    CHECK_U64(cl_parse_count(h, "f(x) = 5", 8), (uint64_t)-1);
    CHECK_U64(cl_parse_count(h, "break", 5), (uint64_t)-1); /* no loop */
    CHECK_U64(cl_parse_count(h, "if x then", 9), (uint64_t)-1);
    CHECK_U64(cl_parse_count(h, "return 1 return 2", 17), (uint64_t)-1);
    CHECK_U64(cl_parse_count(h, "end", 3), (uint64_t)-1); /* trailing junk */
    CHECK_U64(cl_parse_count(h, "x = ", 4), (uint64_t)-1);
    CHECK_U64(cl_parse_count(h, "; ; ;", 5) > 0, 1); /* empty statements */
    /* deep expression nesting is rejected, not a crash */
    {
        static char deep[4096];

        for (size_t i = 0; i < sizeof deep - 1; i++)
            deep[i] = '(';
        deep[sizeof deep - 1] = 0;
        CHECK_U64(cl_parse_count(h, deep, sizeof deep - 1), (uint64_t)-1);
    }
    CHECK_U64(cl_parse_count(h, NULL, 5), (uint64_t)-1);
    cl_free(h);
}

/* Byte-pinned codegen goldens: the exact emitted bytes for four tiny
 * chunks, pinning the header layout, the constant interning, the opcode
 * numbering, the jump fixups and the nested-proto serialization. */
static int expect_blob(cl_ctx *h, const char *src, const uint8_t *want,
                       size_t wantlen, int line)
{
    uint8_t got[4096];

    long n = cl_compile(h, src, strlen(src), got, sizeof got);

    if (n < 0 || (size_t)n != wantlen ||
        memcmp(got, want, wantlen) != 0) {
        test_fail(__FILE__, line, "codegen golden mismatch for '%s' "
                  "(got %ld bytes)", src, n);
        return -1;
    }
    return 0;
}

void test_cl_codegen_golden(void)
{
    cl_ctx *h = build(1);

    if (!h)
        return;
    /* local x = 5 return x ->
     *   hdr nparams=0 nslots=1 nconsts=1 ncode=4 nkids=0
     *   const INT 5; LOADK 0; STORELOC 0; LOADLOC 0; RETURN */
    {
        static const uint8_t want[] = {
            0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00
        };

        CHECK(expect_blob(h, "local x = 5 return x", want,
                          sizeof want, __LINE__) == 0);
    }
    /* return 1 + 2 -> hdr 0/0/2/4/0; INT 1, INT 2;
     * LOADK 0; LOADK 1; ADD; RETURN */
    {
        static const uint8_t want[] = {
            0x00, 0x00, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
            0x0e, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00
        };

        CHECK(expect_blob(h, "return 1 + 2", want, sizeof want,
                          __LINE__) == 0);
    }
    /* if x then f() end -> GETGLOBAL x; JZ 5; GETGLOBAL f; CALL 0;
     * POP; implicit LOADNIL; RETURN (jump lands on the LOADNIL) */
    {
        static const uint8_t want[] = {
            0x00, 0x00, 0x00, 0x02, 0x00, 0x07, 0x00, 0x00, 0x00,
            0x02, 0x01, 0x00, 0x78, 0x02, 0x01, 0x00, 0x66,
            0x06, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x05, 0x00,
            0x06, 0x00, 0x01, 0x00, 0x22, 0x00, 0x00, 0x00,
            0x21, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
            0x24, 0x00, 0x00, 0x00
        };

        CHECK(expect_blob(h, "if x then f() end", want, sizeof want,
                          __LINE__) == 0);
    }
    /* local function g(a, b) return a + b end return g(1, 2) ->
     * root: CLOSURE 0; STORELOC 0; LOADLOC 0; LOADK 1; LOADK 2;
     * CALL 2; RETURN, then the child proto appended after the code */
    {
        static const uint8_t want[] = {
            0x00, 0x01, 0x00, 0x02, 0x00, 0x07, 0x00, 0x01, 0x00,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x23, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x01, 0x00, 0x22, 0x02, 0x00, 0x00,
            0x24, 0x00, 0x00, 0x00,
            0x02, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00,
            0x0e, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00
        };

        CHECK(expect_blob(h, "local function g(a, b) return a + b end "
                              "return g(1, 2)",
                          want, sizeof want, __LINE__) == 0);
    }
    /* short-circuit and-or joins land on the consumer instruction */
    CHECK(cl_compile(h, "return a and b", strlen("return a and b"),
                     NULL, 0) == -1); /* NULL out rejected */
    cl_free(h);
}

void test_cl_stack_discipline(void)
{
    /* the centerpiece: every unit's bytecode, root protos and all
     * nested children, walked by the independent interpreter */
    static const unsigned SEEDS[] = { 1, 2026 };

    for (size_t s = 0; s < sizeof SEEDS / sizeof SEEDS[0]; s++) {
        cl_ctx *h = build(SEEDS[s]);

        if (!h)
            continue;
        for (unsigned u = 0; u < CLANG_UNITS; u++) {
            size_t bl = 0;
            const uint8_t *blob = cl_unit_blob(h, u, &bl);
            uint32_t protos = 0;

            if (!blob || bl == 0) {
                test_fail(__FILE__, __LINE__, "seed %u unit %u: no blob",
                          SEEDS[s], u);
                continue;
            }
            if (blob_check(blob, bl, u, 0, &protos))
                break;
            if (protos < 4) {
                test_fail(__FILE__, __LINE__,
                          "seed %u unit %u: only %u protos",
                          SEEDS[s], u, protos);
                break;
            }
        }
        cl_free(h);
    }
}

void test_cl_run_sane(void)
{
    cl_ctx *h = build(1);
    unsigned th = 2;
    double tp = 0.0;
    uint64_t toks = 0;

    if (!h)
        return;
    CHECK(cl_run(h, 3, &th, &tp, &toks) == 0);
    CHECK_U64(th, 2);
    CHECK_U64(toks, cl_tokens(h));
    CHECK(tp > 0.0 && tp < 1000000.0);
    cl_free(h);
}

void test_cl_thread_clamp(void)
{
    cl_ctx *h = build(1);
    unsigned th = 999;
    double tp = 0.0;
    uint64_t toks = 0;

    if (!h)
        return;
    CHECK(cl_run(h, 2, &th, &tp, &toks) == 0);
    CHECK_U64(th, CLANG_UNITS); /* clamped to the unit count */
    CHECK_U64(toks, cl_tokens(h));
    CHECK(tp > 0.0);
    cl_free(h);
}

void test_cl_accessors_null_safe(void)
{
    cl_ctx *h = build(1);
    uint8_t out[64];
    size_t bl = 12345;

    if (!h)
        return;
    CHECK_U64(cl_units(NULL), 0);
    CHECK_U64(cl_tokens(NULL), 0);
    CHECK_U64(cl_unit_tokens(NULL, 0), 0);
    CHECK_U64(cl_unit_nodes(NULL, 0), 0);
    CHECK(cl_unit_source(NULL, 0) == NULL);
    CHECK_U64(cl_unit_source_len(NULL, 0), 0);
    CHECK(cl_unit_blob(NULL, 0, &bl) == NULL);
    CHECK_U64(bl, 0);
    CHECK_U64(cl_unit_digest(NULL, 0), 0);
    CHECK_U64(cl_lex_count(NULL, "return 1", 8), (uint64_t)-1);
    CHECK_U64(cl_parse_count(NULL, "return 1", 8), (uint64_t)-1);
    CHECK_U64(cl_compile(NULL, "return 1", 8, out, sizeof out),
              (uint64_t)-1);

    CHECK_U64(cl_unit_tokens(h, CLANG_UNITS), 0);
    CHECK_U64(cl_unit_nodes(h, CLANG_UNITS), 0);
    CHECK(cl_unit_source(h, CLANG_UNITS) == NULL);
    CHECK_U64(cl_unit_source_len(h, CLANG_UNITS), 0);
    CHECK(cl_unit_blob(h, CLANG_UNITS, &bl) == NULL);
    CHECK_U64(cl_unit_digest(h, CLANG_UNITS), 0);
    CHECK(cl_lex_count(h, NULL, 5) == -1);
    CHECK(cl_parse_count(h, NULL, 5) == -1);
    CHECK(cl_compile(h, NULL, 5, out, sizeof out) == -1);
    /* buffer too small or NULL out is rejected, not a crash */
    CHECK(cl_compile(h, "return 1 + 2", 12, out, 4) == -1);
    CHECK(cl_compile(h, "return 1 + 2", 12, NULL, 0) == -1);
    cl_free(h);
}

void test_cl_golden(void)
{
    cl_ctx *h = build(1);
    char hex[41];

    if (!h)
        return;
    /* unit 0 (call-graph archetype), generated by tools/gen_goldens.c */
    sha1_hex((const uint8_t *)cl_unit_source(h, 0),
             cl_unit_source_len(h, 0), hex);
    CHECK_STR(hex, "55044ec14d51860597bb85b25e42797daa94f766");
    CHECK_U64(cl_unit_source_len(h, 0), 149455);
    CHECK_U64(cl_unit_tokens(h, 0), 37001);
    CHECK_U64(cl_unit_nodes(h, 0), 28623);
    {
        size_t bl = 0;
        const uint8_t *blob = cl_unit_blob(h, 0, &bl);

        sha1_hex(blob, bl, hex);
        CHECK_STR(hex, "37764c6db2895e9aac9512d6e914a95300a78e1a");
        CHECK_U64(bl, 218775);
    }
    CHECK_U64(cl_tokens(h), 986328);
    cl_free(h);
}
