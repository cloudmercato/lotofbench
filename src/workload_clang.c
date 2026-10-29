/* LotofBench — Clang workload (workload #6).
 *
 * Mirrors the sixth Geekbench 6 CPU workload: compiling source code with
 * a production toolchain — Geekbench 6 builds the Lua interpreter with
 * Clang. A system compiler cannot be part of a reproducible benchmark
 * (machine-dependent binaries, version-dependent optimisation), so
 * lotofbench reproduces the shape of the compiler pipeline dependency
 *-free, with the same honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of 32 "translation
 *     units" written in a Lua-flavoured subset (locals, functions,
 *     tables, control flow, arithmetic, strings, comments) in six
 *     archetype families (call graph, table building, arithmetic
 *     kernels, string handling, control flow, mixed module) that cycle
 *     the way a real project's files do. Fixed --seed means
 *     byte-identical sources on every machine, so scores are comparable
 *     across runs. The sources are generated off the clock — the
 *     stand-in for checking the project out;
 *   - the full pipeline inside the timed cycle — every unit is lexed
 *     into a token stream, parsed into an AST, and compiled to a stack
 *    -machine bytecode (constants interned, slots allocated, branches
 *     patched, nested function protos serialized). All arithmetic is
 *     integer, so the emitted bytes are identical on every machine;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     unit reproduces its reference token count, AST node count and
 *     SHA-1 digest of the emitted bytecode; any divergence aborts the
 *     run instead of scoring;
 *   - shared-task multi-core through the common harness — the unit set
 *     is split into equal contiguous slices, one per thread.
 *
 * The throughput metric is source tokens: the score measures how many
 * thousands of tokens per second the machine lexes, parses and compiles
 * over the whole unit set. The token count is exact and machine-
 * independent, like the corpus size of workload #1.
 *
 * Deliberate divergences from a real compiler run (all documented in
 * docs/benchmarks.md): no system cc, integer-only arithmetic (no
 * floating-point literals), single-value expressions, no lexical
 * capture (nested functions see their own locals and globals only),
 * line comments only. */

#include "workload_clang.h"

#include "harness.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define NONE32 UINT32_MAX

/* Input-size knobs. The unit count is surfaced by the driver (CLANG_UNITS
 * in the header); these shape how big each translation unit is. The
 * resulting corpus is about 960k tokens and a single-core pass takes
 * 150-250 ms on the reference machine. */
#define CL_TOP_LO   820u  /* top-level statements per unit (min) */
#define CL_TOP_RN   480u  /* ... + rng_below(range) */
#define CL_FUNC_LO  5u    /* statements per function body (min) */
#define CL_FUNC_RN  5u    /* ... + rng_below(range) */

/* Hard limits enforced by the parser and the code generator (errors, not
 * crashes — the test hooks feed arbitrary source). */
#define CL_MAX_DEPTH   64u   /* parse nesting */
#define CL_MAX_PROTOS  256u  /* function protos per unit (nesting <= depth) */
#define CL_MAX_SLOTS   4096u  /* locals per proto (u16 in the encoding) */
#define CL_MAX_CODE    65535u /* instructions per proto (u16 in the encoding) */
#define CL_MAX_CONSTS  65535u
#define CL_MAX_KIDS    65535u

/* --- growable byte buffer ------------------------------------------------------*/

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int oom;
} cbuf;

static void buf_grow(cbuf *b, size_t need)
{
    size_t cap = b->cap ? b->cap : 1024;
    uint8_t *np;

    if (b->oom || b->n + need <= b->cap)
        return;
    while (cap < b->n + need)
        cap *= 2;
    np = realloc(b->p, cap);
    if (!np) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void buf_putn(cbuf *b, const void *s, size_t n)
{
    buf_grow(b, n);
    if (b->oom)
        return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void buf_putc(cbuf *b, uint8_t c)
{
    buf_putn(b, &c, 1);
}

static void buf_puts(cbuf *b, const char *s)
{
    buf_putn(b, s, strlen(s));
}

static void buf_putu(cbuf *b, unsigned v)
{
    char t[12];
    int k = 0;

    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        buf_putc(b, (uint8_t)t[--k]);
}

/* Little-endian writers — the serialized bytecode is byte-exact on every
 * machine by construction. */
static void buf_putu16(cbuf *b, uint16_t v)
{
    buf_putc(b, (uint8_t)(v & 0xFF));
    buf_putc(b, (uint8_t)(v >> 8));
}

static void buf_putu64(cbuf *b, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        buf_putc(b, (uint8_t)((v >> (8 * i)) & 0xFF));
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    size_t ncap = *cap ? *cap : 64;
    void *np;

    while (ncap < need)
        ncap *= 2;
    np = realloc(p, ncap * elem);
    if (np)
        *cap = ncap;
    return np;
}

/* --- the lexer -------------------------------------------------------------------
 *
 * Tokens: names, keywords, integer literals (decimal and 0x hex), string
 * literals (with \\ \" \' \n \t \xHH escapes), the operators of the
 * subset, and `--` line comments. The lexer validates as it goes — an
 * unterminated string, a bad escape or an overflowing literal is a
 * lexer error. Tokens reference the source in place (offset + raw
 * length); nothing is copied. */

enum {
    TK_EOF = 0, TK_NAME, TK_INT, TK_STR, TK_OP, TK_KW
};

/* Keywords, alphabetical; the id is the index + 1. */
static const char *const KEYWORDS[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "if", "local", "nil", "not", "or", "repeat", "return",
    "then", "true", "until", "while"
};
#define KW_COUNT     20
#define KW_AND       1
#define KW_BREAK     2
#define KW_DO        3
#define KW_ELSE      4
#define KW_ELSEIF    5
#define KW_END       6
#define KW_FALSE     7
#define KW_FOR       8
#define KW_FUNCTION  9
#define KW_IF        10
#define KW_LOCAL     11
#define KW_NIL       12
#define KW_NOT       13
#define KW_OR        14
#define KW_REPEAT    15
#define KW_RETURN    16
#define KW_THEN      17
#define KW_TRUE      18
#define KW_UNTIL     19
#define KW_WHILE     20

/* Operators. Two-byte ones are matched greedily (`..` before `.`). */
enum {
    LOP_ADD = 1, LOP_SUB, LOP_MUL, LOP_DIV, LOP_MOD, LOP_LEN,
    LOP_EQ, LOP_NE, LOP_LE, LOP_GE, LOP_LT, LOP_GT, LOP_ASSIGN,
    LOP_LP, LOP_RP, LOP_LBRACE, LOP_RBRACE, LOP_LBRACK, LOP_RBRACK,
    LOP_COMMA, LOP_SEMI, LOP_DOT, LOP_CONCAT
};

typedef struct {
    uint8_t kind;
    uint8_t aux;   /* keyword or operator id */
    uint32_t off;  /* byte offset into the source */
    uint32_t len;  /* raw length (strings include the quotes) */
} cl_tok;

/* --- the AST ---------------------------------------------------------------------
 *
 * One flat arena of 24-byte nodes with first/last/next child links, the
 * same shape as the html5 workload's DOM. `aux` and `val` carry the
 * per-kind payload (operator ids, token indexes, flags). Parentheses
 * do not create nodes. */

enum {
    N_NAME, N_INT, N_STR, N_NIL, N_TRUE, N_FALSE,
    N_BINOP, N_UNOP, N_FIELD, N_INDEX, N_CALL, N_TABLE,
    N_FIELDITEM, N_IDXITEM, N_FUNC,
    N_LOCAL, N_ASSIGN, N_EVAL, N_IF, N_WHILE, N_REPEAT, N_FOR,
    N_RETURN, N_BREAK, N_LOCALFUNC, N_GLOBALFUNC, N_BLOCK
};

typedef struct {
    uint16_t kind;
    uint16_t aux;
    uint32_t first, last, next; /* child list */
    uint32_t val;               /* token index / flags */
} cl_node;

/* Binary operators, in precedence order (or < and < comparisons <
 * concat < additive < multiplicative; concat is right-associative). */
enum {
    B_OR = 1, B_AND, B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE,
    B_CONCAT, B_ADD, B_SUB, B_MUL, B_DIV, B_MOD
};

enum { U_NOT = 1, U_NEG, U_LEN };

/* --- the code generator ------------------------------------------------------------*/

/* Stack-machine opcodes. One instruction is 4 bytes: op, a, b (u16 LE).
 * Jumps carry an absolute instruction index. JZ/JNZ pop the tested
 * value. */
enum {
    BC_LOADK = 1, BC_LOADNIL, BC_LOADBOOL, BC_LOADLOC, BC_STORELOC,
    BC_GETGLOBAL, BC_SETGLOBAL, BC_GETFIELD, BC_SETFIELD, BC_GETINDEX,
    BC_SETINDEX, BC_APPEND, BC_NEWTABLE,
    BC_ADD, BC_SUB, BC_MUL, BC_DIV, BC_MOD, BC_CONCAT,
    BC_NOT, BC_UNM, BC_LEN,
    BC_EQ, BC_NE, BC_LT, BC_LE, BC_GT, BC_GE,
    BC_JMP, BC_JZ, BC_JNZ, BC_DUP, BC_POP, BC_CALL, BC_CLOSURE, BC_RETURN
};

/* Constant tags inside a proto's constant table. */
enum { CONST_INT = 1, CONST_STR, CONST_BOOL };

typedef struct {
    uint8_t tag;
    uint64_t i;    /* CONST_INT value / CONST_BOOL 0 or 1 */
    uint32_t off;  /* CONST_STR: offset in the frame's string arena */
    uint16_t len;
} cl_const;

typedef struct {
    uint32_t tok;  /* name token index */
    uint16_t slot;
} cl_scope;

/* One function under compilation: its instruction stream, constant
 * table, open scopes, pending break fixups and child protos. Frames
 * live in a fixed array (no reallocation, so pointers stay valid while
 * children compile); frame indexes are never reused within one
 * compilation, which is what lets a parent serialize a finished child
 * after later siblings have opened. */
typedef struct {
    cbuf code;      /* 4-byte instructions */
    cl_const *consts;
    size_t cn, ccap;
    cbuf strs;      /* interned string bytes */
    cl_scope *scopes;
    size_t sn, scap;
    uint16_t nslots;
    uint32_t *brks; /* break fixups of the innermost open loop */
    size_t bn, bcap;
    uint16_t *kids; /* frame indexes of child protos, in order */
    size_t kn, kcap;
    cbuf ser;       /* this proto's serialized record */
    uint8_t nparams;
} cl_frame;

typedef struct cl_scratch {
    /* lexer output / parser input */
    const uint8_t *src;
    size_t srclen;
    cl_tok *toks;
    size_t toks_n, toks_cap;
    size_t cur;
    /* parser output */
    cl_node *ast;
    size_t ast_n, ast_cap;
    uint32_t root;
    int depth, loop_depth;
    /* shared error state */
    int err, oom;
    /* code generator */
    cl_frame frames[CL_MAX_PROTOS];
    int frame_n;
    cbuf tmp; /* string decode scratch */
} cl_scratch;

static cl_frame *cg_frame(cl_scratch *sc)
{
    return &sc->frames[sc->frame_n - 1];
}

static void frame_reset(cl_frame *f)
{
    f->code.n = 0;
    f->cn = 0;
    f->strs.n = 0;
    f->sn = 0;
    f->nslots = 0;
    f->bn = 0;
    f->kn = 0;
    f->ser.n = 0;
    f->nparams = 0;
}

static void scratch_reset(cl_scratch *sc)
{
    sc->toks_n = 0;
    sc->cur = 0;
    sc->ast_n = 0;
    sc->root = NONE32;
    sc->depth = 0;
    sc->loop_depth = 0;
    sc->err = 0;
    sc->oom = 0;
    sc->frame_n = 0;
    sc->tmp.n = 0;
}

static void scratch_free(cl_scratch *sc)
{
    free(sc->toks);
    free(sc->ast);
    for (int i = 0; i < (int)CL_MAX_PROTOS; i++) {
        cl_frame *f = &sc->frames[i];

        free(f->code.p);
        free(f->consts);
        free(f->strs.p);
        free(f->scopes);
        free(f->brks);
        free(f->kids);
        free(f->ser.p);
    }
    free(sc->tmp.p);
    memset(sc, 0, sizeof(*sc));
}

/* --- lexer helpers ------------------------------------------------------------------*/

static int is_name_start(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_name_char(uint8_t c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static int is_hex_digit(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

static int keyword_of(const uint8_t *s, size_t n)
{
    for (int k = 0; k < KW_COUNT; k++)
        if (strlen(KEYWORDS[k]) == n && memcmp(KEYWORDS[k], s, n) == 0)
            return k + 1;
    return 0;
}

static int toks_push(cl_scratch *sc, uint8_t kind, uint8_t aux,
                     uint32_t off, uint32_t len)
{
    if (sc->toks_n >= sc->toks_cap) {
        cl_tok *t = grow(sc->toks, &sc->toks_cap, sc->toks_n + 1,
                         sizeof(*t));
        if (!t) {
            sc->oom = 1;
            sc->err = 1;
            return -1;
        }
        sc->toks = t;
    }
    cl_tok *tk = &sc->toks[sc->toks_n++];

    tk->kind = kind;
    tk->aux = aux;
    tk->off = off;
    tk->len = len;
    return 0;
}

static int lex_chunk(cl_scratch *sc, const uint8_t *src, size_t len)
{
    size_t i = 0;

    sc->src = src;
    sc->srclen = len;
    sc->toks_n = 0;
    while (i < len) {
        uint8_t c = src[i];
        uint32_t start;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c == '-' && i + 1 < len && src[i + 1] == '-') { /* comment */
            i += 2;
            while (i < len && src[i] != '\n')
                i++;
            continue;
        }
        start = (uint32_t)i;
        if (is_name_start(c)) {
            while (i < len && is_name_char(src[i]))
                i++;
            int kw = keyword_of(src + start, i - start);

            if (kw) {
                if (toks_push(sc, TK_KW, (uint8_t)kw, start,
                              (uint32_t)(i - start)))
                    return -1;
            } else if (toks_push(sc, TK_NAME, 0, start,
                                 (uint32_t)(i - start))) {
                return -1;
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            uint64_t v = 0;

            if (c == '0' && i + 1 < len &&
                (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                if (i >= len || !is_hex_digit(src[i])) {
                    sc->err = 1;
                    return -1;
                }
                for (; i < len && is_hex_digit(src[i]); i++) {
                    if (v > (UINT64_MAX >> 4)) {
                        sc->err = 1;
                        return -1;
                    }
                    v = v * 16 + (uint64_t)hex_value(src[i]);
                }
            } else {
                for (; i < len && src[i] >= '0' && src[i] <= '9'; i++) {
                    unsigned d = (unsigned)(src[i] - '0');

                    if (v > (UINT64_MAX - d) / 10) {
                        sc->err = 1;
                        return -1;
                    }
                    v = v * 10 + d;
                }
            }
            if (i < len && (src[i] == '.' || is_name_char(src[i]))) {
                /* 123abc or 1.5 — no floats, and a digit run must not
                 * touch a '.' (write `1 .. 2` with spaces) */
                sc->err = 1;
                return -1;
            }
            if (toks_push(sc, TK_INT, 0, start, (uint32_t)(i - start)))
                return -1;
            continue;
        }
        if (c == '"') {
            i++;
            for (;;) {
                if (i >= len || src[i] == '\n') {
                    sc->err = 1;
                    return -1;
                }
                if (src[i] == '"') {
                    i++;
                    break;
                }
                if (src[i] == '\\') {
                    uint8_t e;

                    if (i + 1 >= len) {
                        sc->err = 1;
                        return -1;
                    }
                    e = src[i + 1];
                    if (e == '\\' || e == '"' || e == '\'' ||
                        e == 'n' || e == 't') {
                        i += 2;
                    } else if (e == 'x') {
                        if (i + 3 >= len || !is_hex_digit(src[i + 2]) ||
                            !is_hex_digit(src[i + 3])) {
                            sc->err = 1;
                            return -1;
                        }
                        i += 4;
                    } else {
                        sc->err = 1;
                        return -1;
                    }
                } else {
                    i++;
                }
            }
            if (toks_push(sc, TK_STR, 0, start, (uint32_t)(i - start)))
                return -1;
            continue;
        }
        /* operators */
        {
            uint8_t op = 0;

            switch (c) {
            case '+': op = LOP_ADD; i++; break;
            case '-': op = LOP_SUB; i++; break;
            case '*': op = LOP_MUL; i++; break;
            case '/': op = LOP_DIV; i++; break;
            case '%': op = LOP_MOD; i++; break;
            case '#': op = LOP_LEN; i++; break;
            case '=':
                if (i + 1 < len && src[i + 1] == '=') {
                    op = LOP_EQ;
                    i += 2;
                } else {
                    op = LOP_ASSIGN;
                    i++;
                }
                break;
            case '~':
                if (i + 1 < len && src[i + 1] == '=') {
                    op = LOP_NE;
                    i += 2;
                } else {
                    sc->err = 1;
                    return -1;
                }
                break;
            case '<':
                if (i + 1 < len && src[i + 1] == '=') {
                    op = LOP_LE;
                    i += 2;
                } else {
                    op = LOP_LT;
                    i++;
                }
                break;
            case '>':
                if (i + 1 < len && src[i + 1] == '=') {
                    op = LOP_GE;
                    i += 2;
                } else {
                    op = LOP_GT;
                    i++;
                }
                break;
            case '(': op = LOP_LP; i++; break;
            case ')': op = LOP_RP; i++; break;
            case '{': op = LOP_LBRACE; i++; break;
            case '}': op = LOP_RBRACE; i++; break;
            case '[': op = LOP_LBRACK; i++; break;
            case ']': op = LOP_RBRACK; i++; break;
            case ',': op = LOP_COMMA; i++; break;
            case ';': op = LOP_SEMI; i++; break;
            case '.':
                if (i + 1 < len && src[i + 1] == '.') {
                    op = LOP_CONCAT;
                    i += 2;
                } else {
                    op = LOP_DOT;
                    i++;
                }
                break;
            default:
                sc->err = 1;
                return -1;
            }
            if (toks_push(sc, TK_OP, op, start, (uint32_t)(i - start)))
                return -1;
        }
    }
    return 0;
}

/* --- the parser ----------------------------------------------------------------------
 *
 * Recursive descent over the token array, building the node arena.
 * Every construct that can nest bumps sc->depth; past CL_MAX_DEPTH the
 * parse fails instead of blowing the C stack. Statements chain through
 * `next` inside N_BLOCK nodes; return must end its block; break needs
 * an open loop. */

static uint32_t node_new(cl_scratch *sc, uint16_t kind, uint16_t aux,
                         uint32_t val)
{
    if (sc->ast_n >= sc->ast_cap) {
        cl_node *a = grow(sc->ast, &sc->ast_cap, sc->ast_n + 1,
                          sizeof(*a));
        if (!a) {
            sc->oom = 1;
            sc->err = 1;
            return NONE32;
        }
        sc->ast = a;
    }
    cl_node *n = &sc->ast[sc->ast_n];

    n->kind = kind;
    n->aux = aux;
    n->first = NONE32;
    n->last = NONE32;
    n->next = NONE32;
    n->val = val;
    return sc->ast_n++;
}

static void node_add(cl_scratch *sc, uint32_t p, uint32_t c)
{
    cl_node *pn = &sc->ast[p];

    if (pn->first == NONE32)
        pn->first = c;
    else
        sc->ast[pn->last].next = c;
    pn->last = c;
}

static const cl_tok *tok_at(const cl_scratch *sc, size_t i)
{
    static const cl_tok eof = { TK_EOF, 0, 0, 0 };

    return i < sc->toks_n ? &sc->toks[i] : &eof;
}

static int p_at(const cl_scratch *sc, uint8_t kind, uint8_t aux)
{
    const cl_tok *t = tok_at(sc, sc->cur);

    return !sc->err && t->kind == kind && t->aux == aux;
}

static int p_atkw(const cl_scratch *sc, uint8_t kw)
{
    return p_at(sc, TK_KW, kw);
}

static int p_atop(const cl_scratch *sc, uint8_t op)
{
    return p_at(sc, TK_OP, op);
}

static void p_adv(cl_scratch *sc)
{
    if (!sc->err && sc->cur < sc->toks_n)
        sc->cur++;
}

static int p_expectop(cl_scratch *sc, uint8_t op)
{
    if (!p_atop(sc, op)) {
        sc->err = 1;
        return -1;
    }
    p_adv(sc);
    return 0;
}

static int p_expectkw(cl_scratch *sc, uint8_t kw)
{
    if (!p_atkw(sc, kw)) {
        sc->err = 1;
        return -1;
    }
    p_adv(sc);
    return 0;
}

static int p_expectname(cl_scratch *sc, uint32_t *toki)
{
    const cl_tok *t = tok_at(sc, sc->cur);

    if (sc->err || t->kind != TK_NAME) {
        sc->err = 1;
        return -1;
    }
    *toki = (uint32_t)sc->cur;
    p_adv(sc);
    return 0;
}

static int binop_prec(int b)
{
    switch (b) {
    case B_OR: return 1;
    case B_AND: return 2;
    case B_EQ: case B_NE: case B_LT: case B_LE: case B_GT: case B_GE:
        return 3;
    case B_CONCAT: return 4;
    case B_ADD: case B_SUB: return 5;
    default: return 6; /* B_MUL, B_DIV, B_MOD */
    }
}

/* The binary operator a token denotes in an operand position, 0 if none. */
static int binop_of(const cl_tok *t)
{
    if (t->kind == TK_KW) {
        if (t->aux == KW_AND)
            return B_AND;
        if (t->aux == KW_OR)
            return B_OR;
        return 0;
    }
    if (t->kind != TK_OP)
        return 0;
    switch (t->aux) {
    case LOP_EQ: return B_EQ;
    case LOP_NE: return B_NE;
    case LOP_LT: return B_LT;
    case LOP_LE: return B_LE;
    case LOP_GT: return B_GT;
    case LOP_GE: return B_GE;
    case LOP_ADD: return B_ADD;
    case LOP_SUB: return B_SUB;
    case LOP_MUL: return B_MUL;
    case LOP_DIV: return B_DIV;
    case LOP_MOD: return B_MOD;
    case LOP_CONCAT: return B_CONCAT;
    default: return 0;
    }
}

static uint32_t p_expr(cl_scratch *sc);
static uint32_t p_block(cl_scratch *sc);
static int p_funcbody_into(cl_scratch *sc, uint32_t f);

static uint32_t p_tablector(cl_scratch *sc)
{
    if (p_expectop(sc, LOP_LBRACE))
        return NONE32;
    uint32_t t = node_new(sc, N_TABLE, 0, 0);

    if (t == NONE32)
        return NONE32;
    uint16_t nitems = 0;

    if (!p_atop(sc, LOP_RBRACE)) {
        for (;;) {
            uint32_t item;

            if (p_at(sc, TK_NAME, 0) &&
                tok_at(sc, sc->cur + 1)->kind == TK_OP &&
                tok_at(sc, sc->cur + 1)->aux == LOP_ASSIGN) {
                uint32_t ntok;

                if (p_expectname(sc, &ntok))
                    break;
                p_adv(sc); /* '=' */
                uint32_t v = p_expr(sc);

                if (sc->err)
                    break;
                item = node_new(sc, N_FIELDITEM, 0, ntok);
                if (item == NONE32)
                    break;
                node_add(sc, item, v);
            } else if (p_atop(sc, LOP_LBRACK)) {
                p_adv(sc);
                uint32_t k = p_expr(sc);

                if (sc->err)
                    break;
                if (p_expectop(sc, LOP_RBRACK) ||
                    p_expectop(sc, LOP_ASSIGN))
                    break;
                uint32_t v = p_expr(sc);

                if (sc->err)
                    break;
                item = node_new(sc, N_IDXITEM, 0, 0);
                if (item == NONE32)
                    break;
                node_add(sc, item, k);
                node_add(sc, item, v);
            } else {
                item = p_expr(sc);
                if (sc->err)
                    break;
            }
            node_add(sc, t, item);
            if (nitems == 65535) {
                sc->err = 1;
                break;
            }
            nitems++;
            if (p_atop(sc, LOP_COMMA) || p_atop(sc, LOP_SEMI)) {
                p_adv(sc);
                if (p_atop(sc, LOP_RBRACE)) /* trailing separator */
                    break;
                continue;
            }
            break;
        }
    }
    if (!sc->err)
        p_expectop(sc, LOP_RBRACE);
    if (sc->err)
        return NONE32;
    sc->ast[t].aux = nitems;
    return t;
}

static uint32_t p_primary(cl_scratch *sc)
{
    if (sc->err)
        return NONE32;
    const cl_tok *t = tok_at(sc, sc->cur);

    if (t->kind == TK_NAME) {
        uint32_t n = node_new(sc, N_NAME, 0, (uint32_t)sc->cur);

        p_adv(sc);
        return n;
    }
    if (t->kind == TK_INT) {
        uint32_t n = node_new(sc, N_INT, 0, (uint32_t)sc->cur);

        p_adv(sc);
        return n;
    }
    if (t->kind == TK_STR) {
        uint32_t n = node_new(sc, N_STR, 0, (uint32_t)sc->cur);

        p_adv(sc);
        return n;
    }
    if (t->kind == TK_KW) {
        uint16_t kind;

        switch (t->aux) {
        case KW_NIL: kind = N_NIL; break;
        case KW_TRUE: kind = N_TRUE; break;
        case KW_FALSE: kind = N_FALSE; break;
        case KW_FUNCTION:
            p_adv(sc);
            {
                uint32_t f = node_new(sc, N_FUNC, 0, 0);

                if (f == NONE32 || p_funcbody_into(sc, f))
                    return NONE32;
                return f;
            }
        default:
            sc->err = 1;
            return NONE32;
        }
        {
            uint32_t n = node_new(sc, kind, 0, 0);

            p_adv(sc);
            return n;
        }
    }
    if (t->kind == TK_OP && t->aux == LOP_LP) {
        p_adv(sc);
        uint32_t e = p_expr(sc);

        if (!sc->err)
            p_expectop(sc, LOP_RP);
        return sc->err ? NONE32 : e;
    }
    if (t->kind == TK_OP && t->aux == LOP_LBRACE)
        return p_tablector(sc);
    sc->err = 1;
    return NONE32;
}

static uint32_t p_suffixed(cl_scratch *sc)
{
    if (sc->err)
        return NONE32;
    if (++sc->depth > (int)CL_MAX_DEPTH) {
        sc->err = 1;
        return NONE32;
    }
    uint32_t e = p_primary(sc);

    while (!sc->err) {
        if (p_atop(sc, LOP_DOT)) {
            p_adv(sc);
            uint32_t ntok;

            if (p_expectname(sc, &ntok))
                break;
            uint32_t n = node_new(sc, N_FIELD, 0, ntok);

            if (n == NONE32)
                break;
            node_add(sc, n, e);
            e = n;
            continue;
        }
        if (p_atop(sc, LOP_LBRACK)) {
            p_adv(sc);
            uint32_t key = p_expr(sc);

            if (sc->err)
                break;
            if (p_expectop(sc, LOP_RBRACK))
                break;
            uint32_t n = node_new(sc, N_INDEX, 0, 0);

            if (n == NONE32)
                break;
            node_add(sc, n, e);
            node_add(sc, n, key);
            e = n;
            continue;
        }
        if (p_atop(sc, LOP_LP)) {
            p_adv(sc);
            uint32_t call = node_new(sc, N_CALL, 0, 0);

            if (call == NONE32)
                break;
            node_add(sc, call, e);
            uint16_t nargs = 0;

            if (!p_atop(sc, LOP_RP)) {
                for (;;) {
                    uint32_t a = p_expr(sc);

                    if (sc->err)
                        break;
                    node_add(sc, call, a);
                    if (nargs == 65535) {
                        sc->err = 1;
                        break;
                    }
                    nargs++;
                    if (p_atop(sc, LOP_COMMA)) {
                        p_adv(sc);
                        continue;
                    }
                    break;
                }
            }
            if (!sc->err)
                p_expectop(sc, LOP_RP);
            if (sc->err)
                break;
            sc->ast[call].aux = nargs;
            e = call;
            continue;
        }
        break;
    }
    sc->depth--;
    return sc->err ? NONE32 : e;
}

static uint32_t p_unary(cl_scratch *sc)
{
    if (sc->err)
        return NONE32;
    int op = 0;

    if (p_atkw(sc, KW_NOT))
        op = U_NOT;
    else if (p_atop(sc, LOP_SUB))
        op = U_NEG;
    else if (p_atop(sc, LOP_LEN))
        op = U_LEN;
    if (!op)
        return p_suffixed(sc);
    p_adv(sc);
    uint32_t operand = p_unary(sc);

    if (sc->err)
        return NONE32;
    uint32_t n = node_new(sc, N_UNOP, (uint16_t)op, 0);

    if (n == NONE32)
        return NONE32;
    node_add(sc, n, operand);
    return n;
}

static uint32_t p_binop(cl_scratch *sc, int min_prec)
{
    if (sc->err)
        return NONE32;
    if (++sc->depth > (int)CL_MAX_DEPTH) {
        sc->err = 1;
        return NONE32;
    }
    uint32_t lhs = p_unary(sc);

    while (!sc->err) {
        const cl_tok *t = tok_at(sc, sc->cur);
        int op = binop_of(t);

        if (op == 0 || binop_prec(op) < min_prec)
            break;
        p_adv(sc);
        uint32_t rhs = op == B_CONCAT
                           ? p_binop(sc, binop_prec(op))
                           : p_binop(sc, binop_prec(op) + 1);

        if (sc->err)
            break;
        uint32_t n = node_new(sc, N_BINOP, (uint16_t)op, 0);

        if (n == NONE32)
            break;
        node_add(sc, n, lhs);
        node_add(sc, n, rhs);
        lhs = n;
    }
    sc->depth--;
    return sc->err ? NONE32 : lhs;
}

static uint32_t p_expr(cl_scratch *sc)
{
    return p_binop(sc, 1);
}

static int is_lvalue(const cl_scratch *sc, uint32_t e)
{
    uint16_t k = sc->ast[e].kind;

    return k == N_NAME || k == N_FIELD || k == N_INDEX;
}

/* Parse `( params ) block end` into `f` (children: params, block). */
static int p_funcbody_into(cl_scratch *sc, uint32_t f)
{
    if (p_expectop(sc, LOP_LP))
        return -1;
    uint16_t nparams = 0;

    if (!p_atop(sc, LOP_RP)) {
        for (;;) {
            uint32_t ntok;

            if (p_expectname(sc, &ntok))
                return -1;
            uint32_t p = node_new(sc, N_NAME, 0, ntok);

            if (p == NONE32)
                return -1;
            node_add(sc, f, p);
            if (nparams == 255) {
                sc->err = 1;
                return -1;
            }
            nparams++;
            if (p_atop(sc, LOP_COMMA)) {
                p_adv(sc);
                continue;
            }
            break;
        }
    }
    if (p_expectop(sc, LOP_RP))
        return -1;
    uint32_t body = p_block(sc);

    if (sc->err)
        return -1;
    node_add(sc, f, body);
    if (p_expectkw(sc, KW_END))
        return -1;
    sc->ast[f].aux = nparams;
    return 0;
}

static int block_ender(const cl_scratch *sc)
{
    const cl_tok *t = tok_at(sc, sc->cur);

    if (t->kind == TK_EOF)
        return 1;
    if (t->kind != TK_KW)
        return 0;
    return t->aux == KW_END || t->aux == KW_ELSE || t->aux == KW_ELSEIF ||
           t->aux == KW_UNTIL;
}

static uint32_t p_stmt(cl_scratch *sc)
{
    if (sc->err)
        return NONE32;
    const cl_tok *t = tok_at(sc, sc->cur);

    if (t->kind == TK_OP && t->aux == LOP_SEMI) { /* empty statement */
        p_adv(sc);
        return NONE32;
    }

    if (t->kind == TK_KW) {
        switch (t->aux) {
        case KW_LOCAL: {
            p_adv(sc);
            if (p_atkw(sc, KW_FUNCTION)) {
                p_adv(sc);
                uint32_t ntok;

                if (p_expectname(sc, &ntok))
                    return NONE32;
                uint32_t n = node_new(sc, N_LOCALFUNC, 0, 0);

                if (n == NONE32)
                    return NONE32;
                uint32_t nn = node_new(sc, N_NAME, 0, ntok);

                if (nn == NONE32)
                    return NONE32;
                node_add(sc, n, nn);
                if (p_funcbody_into(sc, n))
                    return NONE32;
                return n;
            }
            uint32_t n = node_new(sc, N_LOCAL, 0, 0);

            if (n == NONE32)
                return NONE32;
            uint16_t nnames = 0;

            for (;;) {
                uint32_t ntok;

                if (p_expectname(sc, &ntok))
                    return NONE32;
                uint32_t p = node_new(sc, N_NAME, 0, ntok);

                if (p == NONE32)
                    return NONE32;
                node_add(sc, n, p);
                if (nnames == 65535) {
                    sc->err = 1;
                    return NONE32;
                }
                nnames++;
                if (p_atop(sc, LOP_COMMA)) {
                    p_adv(sc);
                    continue;
                }
                break;
            }
            if (p_atop(sc, LOP_ASSIGN)) {
                p_adv(sc);
                for (;;) {
                    uint32_t v = p_expr(sc);

                    if (sc->err)
                        return NONE32;
                    node_add(sc, n, v);
                    if (p_atop(sc, LOP_COMMA)) {
                        p_adv(sc);
                        continue;
                    }
                    break;
                }
            }
            sc->ast[n].aux = nnames;
            return n;
        }
        case KW_FUNCTION: {
            p_adv(sc);
            uint32_t ntok;

            if (p_expectname(sc, &ntok))
                return NONE32;
            uint32_t n = node_new(sc, N_GLOBALFUNC, 0, 0);

            if (n == NONE32)
                return NONE32;
            uint32_t nn = node_new(sc, N_NAME, 0, ntok);

            if (nn == NONE32)
                return NONE32;
            node_add(sc, n, nn);
            if (p_funcbody_into(sc, n))
                return NONE32;
            return n;
        }
        case KW_IF: {
            p_adv(sc);
            uint32_t n = node_new(sc, N_IF, 0, 0);

            if (n == NONE32)
                return NONE32;
            uint16_t branches = 0;
            int haselse = 0;

            for (;;) {
                uint32_t cond = p_expr(sc);

                if (sc->err)
                    return NONE32;
                if (p_expectkw(sc, KW_THEN))
                    return NONE32;
                uint32_t body = p_block(sc);

                if (sc->err)
                    return NONE32;
                node_add(sc, n, cond);
                node_add(sc, n, body);
                branches++;
                if (p_atkw(sc, KW_ELSEIF)) {
                    p_adv(sc);
                    continue;
                }
                if (p_atkw(sc, KW_ELSE)) {
                    p_adv(sc);
                    uint32_t eb = p_block(sc);

                    if (sc->err)
                        return NONE32;
                    node_add(sc, n, eb);
                    haselse = 1;
                }
                break;
            }
            if (p_expectkw(sc, KW_END))
                return NONE32;
            sc->ast[n].aux = branches;
            sc->ast[n].val = (uint32_t)haselse;
            return n;
        }
        case KW_WHILE: {
            p_adv(sc);
            uint32_t cond = p_expr(sc);

            if (sc->err)
                return NONE32;
            if (p_expectkw(sc, KW_DO))
                return NONE32;
            sc->loop_depth++;
            uint32_t body = p_block(sc);

            sc->loop_depth--;
            if (sc->err)
                return NONE32;
            if (p_expectkw(sc, KW_END))
                return NONE32;
            uint32_t n = node_new(sc, N_WHILE, 0, 0);

            if (n == NONE32)
                return NONE32;
            node_add(sc, n, cond);
            node_add(sc, n, body);
            return n;
        }
        case KW_REPEAT: {
            p_adv(sc);
            sc->loop_depth++;
            uint32_t body = p_block(sc);

            sc->loop_depth--;
            if (sc->err)
                return NONE32;
            if (p_expectkw(sc, KW_UNTIL))
                return NONE32;
            uint32_t cond = p_expr(sc);

            if (sc->err)
                return NONE32;
            uint32_t n = node_new(sc, N_REPEAT, 0, 0);

            if (n == NONE32)
                return NONE32;
            node_add(sc, n, body);
            node_add(sc, n, cond);
            return n;
        }
        case KW_FOR: {
            p_adv(sc);
            uint32_t ntok;

            if (p_expectname(sc, &ntok))
                return NONE32;
            if (p_expectop(sc, LOP_ASSIGN))
                return NONE32;
            uint32_t einit = p_expr(sc);

            if (sc->err)
                return NONE32;
            if (p_expectop(sc, LOP_COMMA))
                return NONE32;
            uint32_t elim = p_expr(sc);

            if (sc->err)
                return NONE32;
            int hasstep = 0;
            uint32_t estep = NONE32;

            if (p_atop(sc, LOP_COMMA)) {
                p_adv(sc);
                estep = p_expr(sc);
                if (sc->err)
                    return NONE32;
                hasstep = 1;
            }
            if (p_expectkw(sc, KW_DO))
                return NONE32;
            sc->loop_depth++;
            uint32_t body = p_block(sc);

            sc->loop_depth--;
            if (sc->err)
                return NONE32;
            if (p_expectkw(sc, KW_END))
                return NONE32;
            uint32_t n = node_new(sc, N_FOR, 0, (uint32_t)hasstep);

            if (n == NONE32)
                return NONE32;
            uint32_t nm = node_new(sc, N_NAME, 0, ntok);

            if (nm == NONE32)
                return NONE32;
            node_add(sc, n, nm);
            node_add(sc, n, einit);
            node_add(sc, n, elim);
            if (hasstep)
                node_add(sc, n, estep);
            node_add(sc, n, body);
            return n;
        }
        case KW_RETURN: {
            p_adv(sc);
            uint32_t n = node_new(sc, N_RETURN, 0, 0);

            if (n == NONE32)
                return NONE32;
            if (!block_ender(sc) && !p_atop(sc, LOP_SEMI)) {
                uint32_t v = p_expr(sc);

                if (sc->err)
                    return NONE32;
                node_add(sc, n, v);
                sc->ast[n].val = 1;
            }
            return n;
        }
        case KW_BREAK: {
            if (!sc->loop_depth) {
                sc->err = 1;
                return NONE32;
            }
            p_adv(sc);
            return node_new(sc, N_BREAK, 0, 0);
        }
        default:
            sc->err = 1;
            return NONE32;
        }
    }

    if (t->kind == TK_NAME) {
        uint32_t e = p_suffixed(sc);

        if (sc->err)
            return NONE32;
        if (p_atop(sc, LOP_COMMA)) { /* lvalue list */
            uint32_t n = node_new(sc, N_ASSIGN, 0, 0);

            if (n == NONE32)
                return NONE32;
            uint16_t nlv = 0;

            for (;;) {
                if (!is_lvalue(sc, e)) {
                    sc->err = 1;
                    return NONE32;
                }
                node_add(sc, n, e);
                if (nlv == 65535) {
                    sc->err = 1;
                    return NONE32;
                }
                nlv++;
                if (!p_atop(sc, LOP_COMMA))
                    break;
                p_adv(sc);
                e = p_suffixed(sc);
                if (sc->err)
                    return NONE32;
            }
            if (p_expectop(sc, LOP_ASSIGN))
                return NONE32;
            for (;;) {
                uint32_t v = p_expr(sc);

                if (sc->err)
                    return NONE32;
                node_add(sc, n, v);
                if (p_atop(sc, LOP_COMMA)) {
                    p_adv(sc);
                    continue;
                }
                break;
            }
            sc->ast[n].aux = nlv;
            return n;
        }
        if (p_atop(sc, LOP_ASSIGN)) {
            if (!is_lvalue(sc, e)) {
                sc->err = 1;
                return NONE32;
            }
            p_adv(sc);
            uint32_t v = p_expr(sc);

            if (sc->err)
                return NONE32;
            uint32_t n = node_new(sc, N_ASSIGN, 1, 0);

            if (n == NONE32)
                return NONE32;
            node_add(sc, n, e);
            node_add(sc, n, v);
            return n;
        }
        if (sc->ast[e].kind != N_CALL) { /* only calls are statements */
            sc->err = 1;
            return NONE32;
        }
        uint32_t n = node_new(sc, N_EVAL, 0, 0);

        if (n == NONE32)
            return NONE32;
        node_add(sc, n, e);
        return n;
    }

    sc->err = 1;
    return NONE32;
}

static uint32_t p_block(cl_scratch *sc)
{
    if (sc->err)
        return NONE32;
    uint32_t b = node_new(sc, N_BLOCK, 0, 0);

    if (b == NONE32)
        return NONE32;
    uint32_t head = NONE32, tail = NONE32;

    while (!sc->err && !block_ender(sc)) {
        uint32_t s = p_stmt(sc);

        if (sc->err)
            break;
        if (s == NONE32)
            continue; /* empty statement */
        if (head == NONE32)
            head = s;
        else
            sc->ast[tail].next = s;
        tail = s;
        if (sc->ast[s].kind == N_RETURN) {
            /* return must end its block (an optional `;` may follow) */
            if (!block_ender(sc)) {
                if (p_atop(sc, LOP_SEMI))
                    p_adv(sc);
                if (!block_ender(sc))
                    sc->err = 1;
            }
            break;
        }
    }
    if (sc->err)
        return NONE32;
    sc->ast[b].first = head;
    sc->ast[b].last = tail;
    return b;
}

/* --- the code generator ------------------------------------------------------------
 *
 * Walks the AST and emits the stack-machine bytecode. Every statement
 * leaves the stack as it found it; every expression leaves exactly one
 * value; JZ/JNZ pop what they test. That discipline makes every jump
 * target coherent, including the joins of `and`/`or` (the taken branch
 * keeps the left operand, the fall-through has the right one) and the
 * implicit `return nil` appended when a body does not end in return. */

static int emit(cl_scratch *sc, uint8_t op, uint8_t a, uint16_t b)
{
    cl_frame *f = cg_frame(sc);

    buf_putc(&f->code, op);
    buf_putc(&f->code, a);
    buf_putu16(&f->code, b);
    if (f->code.oom) {
        sc->oom = 1;
        sc->err = 1;
        return -1;
    }
    if (f->code.n / 4 > CL_MAX_CODE) {
        sc->err = 1;
        return -1;
    }
    return 0;
}

static uint32_t here(cl_scratch *sc)
{
    return (uint32_t)(cg_frame(sc)->code.n / 4);
}

static void patch(cl_scratch *sc, uint32_t instr, uint16_t target)
{
    cl_frame *f = cg_frame(sc);

    f->code.p[instr * 4 + 2] = (uint8_t)(target & 0xFF);
    f->code.p[instr * 4 + 3] = (uint8_t)(target >> 8);
}

static int slot_reserve(cl_scratch *sc, unsigned n, uint16_t *out)
{
    cl_frame *f = cg_frame(sc);

    if ((unsigned)f->nslots + n > CL_MAX_SLOTS) {
        sc->err = 1;
        return -1;
    }
    *out = f->nslots;
    f->nslots = (uint16_t)(f->nslots + n);
    return 0;
}

static int scope_push(cl_scratch *sc, uint32_t tok, uint16_t slot)
{
    cl_frame *f = cg_frame(sc);

    if (f->sn >= f->scap) {
        cl_scope *s = grow(f->scopes, &f->scap, f->sn + 1, sizeof(*s));

        if (!s) {
            sc->oom = 1;
            sc->err = 1;
            return -1;
        }
        f->scopes = s;
    }
    f->scopes[f->sn].tok = tok;
    f->scopes[f->sn].slot = slot;
    f->sn++;
    return 0;
}

static int tok_eq(const cl_scratch *sc, uint32_t a, uint32_t b)
{
    const cl_tok *ta = &sc->toks[a], *tb = &sc->toks[b];

    return ta->len == tb->len &&
           memcmp(sc->src + ta->off, sc->src + tb->off, ta->len) == 0;
}

static int scope_resolve(cl_scratch *sc, uint32_t tok)
{
    const cl_frame *f = cg_frame(sc);

    for (size_t i = f->sn; i > 0; i--)
        if (tok_eq(sc, f->scopes[i - 1].tok, tok))
            return f->scopes[i - 1].slot;
    return -1;
}

static int const_push(cl_scratch *sc, uint8_t tag, uint64_t i, uint32_t off,
                      uint16_t len, uint16_t *out)
{
    cl_frame *f = cg_frame(sc);

    if (f->cn >= CL_MAX_CONSTS) {
        sc->err = 1;
        return -1;
    }
    if (f->cn >= f->ccap) {
        cl_const *c = grow(f->consts, &f->ccap, f->cn + 1, sizeof(*c));

        if (!c) {
            sc->oom = 1;
            sc->err = 1;
            return -1;
        }
        f->consts = c;
    }
    f->consts[f->cn].tag = tag;
    f->consts[f->cn].i = i;
    f->consts[f->cn].off = off;
    f->consts[f->cn].len = len;
    *out = (uint16_t)f->cn;
    f->cn++;
    return 0;
}

static int intern_int(cl_scratch *sc, uint64_t v, uint16_t *out)
{
    cl_frame *f = cg_frame(sc);

    for (size_t k = 0; k < f->cn; k++)
        if (f->consts[k].tag == CONST_INT && f->consts[k].i == v) {
            *out = (uint16_t)k;
            return 0;
        }
    return const_push(sc, CONST_INT, v, 0, 0, out);
}

static int intern_bytes(cl_scratch *sc, const uint8_t *s, size_t n,
                        uint16_t *out)
{
    cl_frame *f = cg_frame(sc);

    if (n > 65535) {
        sc->err = 1;
        return -1;
    }
    for (size_t k = 0; k < f->cn; k++)
        if (f->consts[k].tag == CONST_STR && f->consts[k].len == n &&
            (n == 0 ||
             memcmp(f->strs.p + f->consts[k].off, s, n) == 0)) {
            *out = (uint16_t)k;
            return 0;
        }
    uint32_t off = (uint32_t)f->strs.n;

    buf_putn(&f->strs, s, n);
    if (f->strs.oom) {
        sc->oom = 1;
        sc->err = 1;
        return -1;
    }
    return const_push(sc, CONST_STR, 0, off, (uint16_t)n, out);
}

static int intern_tok(cl_scratch *sc, uint32_t tok, uint16_t *out)
{
    const cl_tok *t = &sc->toks[tok];

    return intern_bytes(sc, sc->src + t->off, t->len, out);
}

/* Decode a string literal (the lexer validated the escapes) and intern
 * the decoded bytes. */
static int intern_str_tok(cl_scratch *sc, uint32_t tok, uint16_t *out)
{
    const cl_tok *t = &sc->toks[tok];
    const uint8_t *s = sc->src + t->off + 1;
    size_t n = t->len - 2;

    sc->tmp.n = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];

        if (c != '\\') {
            buf_putc(&sc->tmp, c);
            continue;
        }
        uint8_t e = s[++i];

        switch (e) {
        case '\\': case '"': case '\'':
            buf_putc(&sc->tmp, e);
            break;
        case 'n': buf_putc(&sc->tmp, 0x0A); break;
        case 't': buf_putc(&sc->tmp, 0x09); break;
        case 'x':
            buf_putc(&sc->tmp, (uint8_t)(hex_value(s[i + 1]) * 16 +
                                         hex_value(s[i + 2])));
            i += 2;
            break;
        }
    }
    if (sc->tmp.oom) {
        sc->oom = 1;
        sc->err = 1;
        return -1;
    }
    return intern_bytes(sc, sc->tmp.p, sc->tmp.n, out);
}

static uint64_t tok_int_value(const cl_scratch *sc, uint32_t tok)
{
    const cl_tok *t = &sc->toks[tok];
    const uint8_t *s = sc->src + t->off;
    size_t n = t->len;
    uint64_t v = 0;

    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < n; i++)
            v = v * 16 + (uint64_t)hex_value(s[i]);
    } else {
        for (size_t i = 0; i < n; i++)
            v = v * 10 + (uint64_t)(s[i] - '0');
    }
    return v;
}

static int cg_expr(cl_scratch *sc, uint32_t e);
static int cg_block(cl_scratch *sc, uint32_t b);
static int cg_stmt(cl_scratch *sc, uint32_t s);
static int cg_proto(cl_scratch *sc, uint32_t fnode, uint16_t *ord_out);

/* Serialize the current frame: header, constants, code, then the
 * serialized records of its child protos in order. */
static int proto_finish(cl_scratch *sc)
{
    cl_frame *f = cg_frame(sc);

    if (f->cn > CL_MAX_CONSTS || f->code.n / 4 > CL_MAX_CODE ||
        f->kn > CL_MAX_KIDS) {
        sc->err = 1;
        return -1;
    }
    /* implicit `return nil` when the body does not end with a return */
    if (f->code.n == 0 || f->code.p[f->code.n - 4] != BC_RETURN) {
        if (emit(sc, BC_LOADNIL, 0, 0) || emit(sc, BC_RETURN, 0, 0))
            return -1;
    }
    cbuf *s = &f->ser;

    buf_putc(s, f->nparams);
    buf_putu16(s, f->nslots);
    buf_putu16(s, (uint16_t)f->cn);
    buf_putu16(s, (uint16_t)(f->code.n / 4));
    buf_putu16(s, (uint16_t)f->kn);
    if (s->oom) {
        sc->oom = 1;
        sc->err = 1;
        return -1;
    }
    for (size_t k = 0; k < f->cn; k++) {
        const cl_const *c = &f->consts[k];

        buf_putc(s, c->tag);
        if (c->tag == CONST_INT) {
            buf_putu64(s, c->i);
        } else if (c->tag == CONST_STR) {
            buf_putu16(s, c->len);
            buf_putn(s, f->strs.p + c->off, c->len);
        } else {
            buf_putc(s, (uint8_t)c->i);
        }
        if (s->oom) {
            sc->oom = 1;
            sc->err = 1;
            return -1;
        }
    }
    buf_putn(s, f->code.p, f->code.n);
    for (size_t k = 0; k < f->kn; k++) {
        const cl_frame *kid = &sc->frames[f->kids[k]];

        buf_putn(s, kid->ser.p, kid->ser.n);
    }
    if (s->oom) {
        sc->oom = 1;
        sc->err = 1;
        return -1;
    }
    return 0;
}

/* Compile `fnode` (N_FUNC, N_LOCALFUNC or N_GLOBALFUNC: params then
 * block) into a fresh child frame; record it in the current frame's
 * child list and return the child ordinal for BC_CLOSURE. */
static int cg_proto(cl_scratch *sc, uint32_t fnode, uint16_t *ord_out)
{
    cl_frame *parent = cg_frame(sc);

    if (parent->kn >= CL_MAX_KIDS) {
        sc->err = 1;
        return -1;
    }
    if (parent->kn >= parent->kcap) {
        uint16_t *k = grow(parent->kids, &parent->kcap, parent->kn + 1,
                           sizeof(*k));

        if (!k) {
            sc->oom = 1;
            sc->err = 1;
            return -1;
        }
        parent->kids = k;
    }
    if (sc->frame_n >= (int)CL_MAX_PROTOS) {
        sc->err = 1;
        return -1;
    }
    uint16_t ord = (uint16_t)parent->kn;

    parent->kids[parent->kn++] = (uint16_t)sc->frame_n;
    cl_frame *child = &sc->frames[sc->frame_n];

    frame_reset(child);
    child->nparams = sc->ast[fnode].aux;
    sc->frame_n++;
    uint32_t c = sc->ast[fnode].first;

    if (sc->ast[fnode].kind == N_LOCALFUNC ||
        sc->ast[fnode].kind == N_GLOBALFUNC)
        c = sc->ast[c].next; /* skip the function name */
    while (c != NONE32 && sc->ast[c].kind == N_NAME) {
        uint16_t slot;

        if (slot_reserve(sc, 1, &slot) ||
            scope_push(sc, sc->ast[c].val, slot))
            return -1;
        c = sc->ast[c].next;
    }
    int rc = cg_block(sc, c);

    if (rc == 0)
        rc = proto_finish(sc);
    sc->frame_n--; /* close the child; the parent is current again */
    if (rc)
        return -1;
    *ord_out = ord;
    return 0;
}

static int cg_expr(cl_scratch *sc, uint32_t e)
{
    if (sc->err)
        return -1;
    const cl_node *n = &sc->ast[e];

    switch (n->kind) {
    case N_INT: {
        uint16_t k;

        if (intern_int(sc, tok_int_value(sc, n->val), &k))
            return -1;
        return emit(sc, BC_LOADK, 0, k);
    }
    case N_STR: {
        uint16_t k;

        if (intern_str_tok(sc, n->val, &k))
            return -1;
        return emit(sc, BC_LOADK, 0, k);
    }
    case N_NIL:
        return emit(sc, BC_LOADNIL, 0, 0);
    case N_TRUE:
        return emit(sc, BC_LOADBOOL, 1, 0);
    case N_FALSE:
        return emit(sc, BC_LOADBOOL, 0, 0);
    case N_NAME: {
        int slot = scope_resolve(sc, n->val);

        if (slot >= 0)
            return emit(sc, BC_LOADLOC, 0, (uint16_t)slot);
        uint16_t k;

        if (intern_tok(sc, n->val, &k))
            return -1;
        return emit(sc, BC_GETGLOBAL, 0, k);
    }
    case N_FIELD: {
        if (cg_expr(sc, n->first))
            return -1;
        uint16_t k;

        if (intern_tok(sc, n->val, &k))
            return -1;
        return emit(sc, BC_GETFIELD, 0, k);
    }
    case N_INDEX: {
        if (cg_expr(sc, n->first))
            return -1;
        if (cg_expr(sc, sc->ast[n->first].next))
            return -1;
        return emit(sc, BC_GETINDEX, 0, 0);
    }
    case N_CALL: {
        if (cg_expr(sc, n->first))
            return -1;
        for (uint32_t c = sc->ast[n->first].next; c != NONE32;
             c = sc->ast[c].next)
            if (cg_expr(sc, c))
                return -1;
        return emit(sc, BC_CALL, n->aux, 0);
    }
    case N_TABLE: {
        if (emit(sc, BC_NEWTABLE, n->aux, 0))
            return -1;
        for (uint32_t c = n->first; c != NONE32; c = sc->ast[c].next) {
            const cl_node *it = &sc->ast[c];

            if (it->kind == N_FIELDITEM) {
                if (cg_expr(sc, it->first))
                    return -1;
                uint16_t k;

                if (intern_tok(sc, it->val, &k))
                    return -1;
                if (emit(sc, BC_SETFIELD, 0, k))
                    return -1;
            } else if (it->kind == N_IDXITEM) {
                if (cg_expr(sc, it->first))
                    return -1;
                if (cg_expr(sc, sc->ast[it->first].next))
                    return -1;
                if (emit(sc, BC_SETINDEX, 0, 0))
                    return -1;
            } else {
                if (cg_expr(sc, c))
                    return -1;
                if (emit(sc, BC_APPEND, 0, 0))
                    return -1;
            }
        }
        return 0;
    }
    case N_FUNC: {
        uint16_t ord;

        if (cg_proto(sc, e, &ord))
            return -1;
        return emit(sc, BC_CLOSURE, 0, ord);
    }
    case N_UNOP: {
        if (cg_expr(sc, n->first))
            return -1;
        uint8_t op = n->aux == U_NOT ? BC_NOT
                         : n->aux == U_NEG ? BC_UNM : BC_LEN;

        return emit(sc, op, 0, 0);
    }
    case N_BINOP: {
        if (n->aux == B_AND || n->aux == B_OR) {
            /* short-circuit: the taken branch keeps the left operand,
             * the fall-through replaces it with the right one */
            if (cg_expr(sc, n->first))
                return -1;
            if (emit(sc, BC_DUP, 0, 0))
                return -1;
            uint32_t j = here(sc);

            if (emit(sc, n->aux == B_AND ? BC_JZ : BC_JNZ, 0, 0))
                return -1;
            if (emit(sc, BC_POP, 0, 0))
                return -1;
            if (cg_expr(sc, sc->ast[n->first].next))
                return -1;
            patch(sc, j, (uint16_t)here(sc));
            return 0;
        }
        if (cg_expr(sc, n->first))
            return -1;
        if (cg_expr(sc, sc->ast[n->first].next))
            return -1;
        uint8_t op;

        switch (n->aux) {
        case B_ADD: op = BC_ADD; break;
        case B_SUB: op = BC_SUB; break;
        case B_MUL: op = BC_MUL; break;
        case B_DIV: op = BC_DIV; break;
        case B_MOD: op = BC_MOD; break;
        case B_CONCAT: op = BC_CONCAT; break;
        case B_EQ: op = BC_EQ; break;
        case B_NE: op = BC_NE; break;
        case B_LT: op = BC_LT; break;
        case B_LE: op = BC_LE; break;
        case B_GT: op = BC_GT; break;
        default: op = BC_GE; break; /* B_GE */
        }
        return emit(sc, op, 0, 0);
    }
    default:
        sc->err = 1;
        return -1;
    }
}

static int cg_stmt(cl_scratch *sc, uint32_t s)
{
    if (sc->err)
        return -1;
    const cl_node *n = &sc->ast[s];

    switch (n->kind) {
    case N_LOCAL: {
        /* slots are never reused, so the new locals are contiguous;
         * they bind only after the values are emitted (Lua semantics:
         * the initializers still see the outer scope) */
        uint16_t names = n->aux, s0;

        if (slot_reserve(sc, names, &s0))
            return -1;
        uint32_t v = n->first;

        for (uint16_t i = 0; i < names; i++)
            v = sc->ast[v].next;
        for (uint16_t i = 0; i < names; i++) {
            if (v != NONE32) {
                if (cg_expr(sc, v))
                    return -1;
                v = sc->ast[v].next;
            } else if (emit(sc, BC_LOADNIL, 0, 0)) {
                return -1;
            }
            if (emit(sc, BC_STORELOC, 0, (uint16_t)(s0 + i)))
                return -1;
        }
        uint32_t c = n->first;

        for (uint16_t i = 0; i < names; i++) {
            if (scope_push(sc, sc->ast[c].val, (uint16_t)(s0 + i)))
                return -1;
            c = sc->ast[c].next;
        }
        return 0;
    }
    case N_ASSIGN: {
        /* pairwise, left to right: object expression, then value */
        uint16_t nlv = n->aux;
        uint32_t lv = n->first, v = n->first;

        for (uint16_t i = 0; i < nlv; i++)
            v = sc->ast[v].next;
        for (uint16_t i = 0; i < nlv; i++) {
            const cl_node *l = &sc->ast[lv];
            int val_nil = v == NONE32;

            if (l->kind == N_NAME) {
                int slot = scope_resolve(sc, l->val);

                if (slot >= 0) {
                    if (val_nil ? emit(sc, BC_LOADNIL, 0, 0)
                                : cg_expr(sc, v))
                        return -1;
                    if (emit(sc, BC_STORELOC, 0, slot))
                        return -1;
                } else {
                    uint16_t k;

                    if (intern_tok(sc, l->val, &k))
                        return -1;
                    if (val_nil ? emit(sc, BC_LOADNIL, 0, 0)
                                : cg_expr(sc, v))
                        return -1;
                    if (emit(sc, BC_SETGLOBAL, 0, k))
                        return -1;
                }
            } else if (l->kind == N_FIELD) {
                uint16_t k;

                if (cg_expr(sc, l->first) || intern_tok(sc, l->val, &k))
                    return -1;
                if (val_nil ? emit(sc, BC_LOADNIL, 0, 0)
                            : cg_expr(sc, v))
                    return -1;
                /* SETFIELD keeps the table (the constructor relies on
                 * that); an assignment statement is done with it */
                if (emit(sc, BC_SETFIELD, 0, k) || emit(sc, BC_POP, 0, 0))
                    return -1;
            } else { /* N_INDEX */
                if (cg_expr(sc, l->first) ||
                    cg_expr(sc, sc->ast[l->first].next))
                    return -1;
                if (val_nil ? emit(sc, BC_LOADNIL, 0, 0)
                            : cg_expr(sc, v))
                    return -1;
                /* ditto SETINDEX */
                if (emit(sc, BC_SETINDEX, 0, 0) || emit(sc, BC_POP, 0, 0))
                    return -1;
            }
            lv = sc->ast[lv].next;
            if (v != NONE32)
                v = sc->ast[v].next;
        }
        return 0;
    }
    case N_EVAL: {
        if (cg_expr(sc, n->first))
            return -1;
        return emit(sc, BC_POP, 0, 0);
    }
    case N_IF: {
        uint16_t branches = n->aux;
        int haselse = (int)n->val;
        uint32_t jmps[32];
        size_t njmp = 0;
        uint32_t c = n->first;

        if (branches > 32) {
            sc->err = 1;
            return -1;
        }
        for (uint16_t b = 0; b < branches; b++) {
            uint32_t cond = c, body = sc->ast[c].next;

            if (cg_expr(sc, cond))
                return -1;
            uint32_t jz = here(sc);

            if (emit(sc, BC_JZ, 0, 0))
                return -1;
            if (cg_block(sc, body))
                return -1;
            if (b + 1 < branches || haselse) {
                if (njmp >= sizeof jmps / sizeof jmps[0]) {
                    sc->err = 1;
                    return -1;
                }
                jmps[njmp++] = here(sc);
                if (emit(sc, BC_JMP, 0, 0))
                    return -1;
            }
            patch(sc, jz, (uint16_t)here(sc));
            c = sc->ast[body].next;
        }
        if (haselse && cg_block(sc, c))
            return -1;
        uint16_t end = (uint16_t)here(sc);

        for (size_t k = 0; k < njmp; k++)
            patch(sc, jmps[k], end);
        return 0;
    }
    case N_WHILE: {
        uint32_t cond = n->first, body = sc->ast[cond].next;
        uint32_t top = here(sc);

        if (cg_expr(sc, cond))
            return -1;
        uint32_t jz = here(sc);

        if (emit(sc, BC_JZ, 0, 0))
            return -1;
        size_t brk0 = cg_frame(sc)->bn;

        if (cg_block(sc, body))
            return -1;
        if (emit(sc, BC_JMP, 0, (uint16_t)top))
            return -1;
        cl_frame *f = cg_frame(sc);
        uint16_t end = (uint16_t)here(sc);

        patch(sc, jz, end);
        for (size_t k = brk0; k < f->bn; k++)
            patch(sc, f->brks[k], end);
        f->bn = brk0;
        return 0;
    }
    case N_REPEAT: {
        uint32_t body = n->first, cond = sc->ast[body].next;
        uint32_t top = here(sc);
        size_t brk0 = cg_frame(sc)->bn;

        if (cg_block(sc, body))
            return -1;
        if (cg_expr(sc, cond))
            return -1;
        if (emit(sc, BC_JZ, 0, (uint16_t)top))
            return -1;
        cl_frame *f = cg_frame(sc);
        uint16_t end = (uint16_t)here(sc);

        for (size_t k = brk0; k < f->bn; k++)
            patch(sc, f->brks[k], end);
        f->bn = brk0;
        return 0;
    }
    case N_FOR: {
        uint32_t c = n->first; /* the loop variable */
        uint32_t einit = sc->ast[c].next;
        uint32_t elim = sc->ast[einit].next;
        int hasstep = (int)n->val;
        uint32_t estep = hasstep ? sc->ast[elim].next : NONE32;
        uint32_t body = hasstep ? sc->ast[estep].next
                                : sc->ast[elim].next;
        uint16_t sv;

        if (slot_reserve(sc, 3, &sv))
            return -1;
        uint16_t sl = (uint16_t)(sv + 1), ss = (uint16_t)(sv + 2);

        if (cg_expr(sc, einit) ||
            emit(sc, BC_STORELOC, 0, sv))
            return -1;
        if (cg_expr(sc, elim) ||
            emit(sc, BC_STORELOC, 0, sl))
            return -1;
        if (hasstep) {
            if (cg_expr(sc, estep))
                return -1;
        } else {
            uint16_t k;

            if (intern_int(sc, 1, &k) || emit(sc, BC_LOADK, 0, k))
                return -1;
        }
        if (emit(sc, BC_STORELOC, 0, ss))
            return -1;
        size_t mark = cg_frame(sc)->sn;

        if (scope_push(sc, sc->ast[c].val, sv))
            return -1;
        uint32_t top = here(sc);

        if (emit(sc, BC_LOADLOC, 0, sv) ||
            emit(sc, BC_LOADLOC, 0, sl) ||
            emit(sc, BC_LE, 0, 0))
            return -1;
        uint32_t jz = here(sc);

        if (emit(sc, BC_JZ, 0, 0))
            return -1;
        size_t brk0 = cg_frame(sc)->bn;

        if (cg_block(sc, body))
            return -1;
        if (emit(sc, BC_LOADLOC, 0, sv) ||
            emit(sc, BC_LOADLOC, 0, ss) ||
            emit(sc, BC_ADD, 0, 0) ||
            emit(sc, BC_STORELOC, 0, sv) ||
            emit(sc, BC_JMP, 0, (uint16_t)top))
            return -1;
        cl_frame *f = cg_frame(sc);
        uint16_t end = (uint16_t)here(sc);

        patch(sc, jz, end);
        for (size_t k = brk0; k < f->bn; k++)
            patch(sc, f->brks[k], end);
        f->bn = brk0;
        f->sn = mark;
        return 0;
    }
    case N_RETURN: {
        if (n->first != NONE32) {
            if (cg_expr(sc, n->first))
                return -1;
        } else if (emit(sc, BC_LOADNIL, 0, 0)) {
            return -1;
        }
        return emit(sc, BC_RETURN, 0, 0);
    }
    case N_BREAK: {
        cl_frame *f = cg_frame(sc);

        if (f->bn >= f->bcap) {
            uint32_t *b = grow(f->brks, &f->bcap, f->bn + 1, sizeof(*b));

            if (!b) {
                sc->oom = 1;
                sc->err = 1;
                return -1;
            }
            f->brks = b;
        }
        uint32_t j = here(sc);

        if (emit(sc, BC_JMP, 0, 0))
            return -1;
        f->brks[f->bn++] = j;
        return 0;
    }
    case N_LOCALFUNC: {
        uint16_t slot;

        if (slot_reserve(sc, 1, &slot) ||
            scope_push(sc, sc->ast[n->first].val, slot))
            return -1;
        uint16_t ord;

        if (cg_proto(sc, s, &ord))
            return -1;
        if (emit(sc, BC_CLOSURE, 0, ord))
            return -1;
        return emit(sc, BC_STORELOC, 0, slot);
    }
    case N_GLOBALFUNC: {
        uint16_t ord, k;

        if (cg_proto(sc, s, &ord))
            return -1;
        if (emit(sc, BC_CLOSURE, 0, ord) ||
            intern_tok(sc, sc->ast[n->first].val, &k))
            return -1;
        return emit(sc, BC_SETGLOBAL, 0, k);
    }
    default:
        sc->err = 1;
        return -1;
    }
}

static int cg_block(cl_scratch *sc, uint32_t b)
{
    if (sc->err)
        return -1;
    if (b == NONE32 || sc->ast[b].kind != N_BLOCK) {
        sc->err = 1;
        return -1;
    }
    size_t mark = cg_frame(sc)->sn;

    for (uint32_t c = sc->ast[b].first; c != NONE32; c = sc->ast[c].next)
        if (cg_stmt(sc, c))
            return -1;
    cg_frame(sc)->sn = mark;
    return sc->err ? -1 : 0;
}

/* Lex, parse and compile one chunk into sc->frames[0].ser. */
static int cl_compile_scratch(cl_scratch *sc, const uint8_t *src, size_t len)
{
    scratch_reset(sc);
    if (lex_chunk(sc, src, len))
        return -1;
    sc->cur = 0;
    sc->root = p_block(sc);
    if (sc->err)
        return -1;
    if (sc->cur != sc->toks_n) { /* trailing junk (unbalanced end...) */
        sc->err = 1;
        return -1;
    }
    frame_reset(&sc->frames[0]);
    sc->frame_n = 1;
    if (cg_block(sc, sc->root) || proto_finish(sc))
        return -1;
    return 0;
}

/* --- synthetic source generator ------------------------------------------------------
 *
 * Six archetype families — a call-graph module, a table builder, an
 * arithmetic kernel, a string handler, a control-flow module and a
 * mixed module — the way a real project's files differ. Everything is
 * seeded: same --seed, byte-identical sources on every machine. The
 * generator tracks scopes in a name pool so every reference lands on a
 * name that is in scope (function bodies only see globals and their own
 * locals — the no-capture rule of the subset). */

typedef struct {
    char s[25];
    uint8_t kind;   /* 1 var, 2 func, 3 table */
    uint8_t global; /* visible from inside nested function bodies */
} gname;

typedef struct {
    gname v[48];
    unsigned n;
    unsigned marks[16], nmarks; /* block scopes */
    unsigned fbase[8], nfbase;  /* function-body bases */
    unsigned fdepth;
} gnames;

static const char GCON[] = "bcdfgklmnprstvz";
static const char GVO[] = "aeiou";
static const char GFIELDS[8][7] = {
    "key", "val", "next", "size", "name", "left", "right", "kind"
};

static void pool_seed(gnames *nm)
{
    memset(nm, 0, sizeof(*nm));
    strcpy(nm->v[nm->n].s, "print");
    nm->v[nm->n].kind = 2;
    nm->v[nm->n].global = 1;
    nm->n++;
    strcpy(nm->v[nm->n].s, "g_tab");
    nm->v[nm->n].kind = 3;
    nm->v[nm->n].global = 1;
    nm->n++;
    strcpy(nm->v[nm->n].s, "g_count");
    nm->v[nm->n].kind = 1;
    nm->v[nm->n].global = 1;
    nm->n++;
    strcpy(nm->v[nm->n].s, "g_label");
    nm->v[nm->n].kind = 1;
    nm->v[nm->n].global = 1;
    nm->n++;
}

static void pool_enter(gnames *nm)
{
    if (nm->nmarks < sizeof nm->marks / sizeof nm->marks[0])
        nm->marks[nm->nmarks++] = nm->n;
}

static void pool_leave(gnames *nm)
{
    if (nm->nmarks)
        nm->n = nm->marks[--nm->nmarks];
}

static void pool_fenter(gnames *nm)
{
    if (nm->nfbase < sizeof nm->fbase / sizeof nm->fbase[0]) {
        nm->fbase[nm->nfbase++] = nm->n;
        nm->fdepth++;
    }
}

static void pool_fleave(gnames *nm)
{
    if (nm->nfbase) {
        nm->nfbase--;
        nm->fdepth--;
    }
}

static int pool_pick(rng_t *r, gnames *nm, unsigned kind_mask)
{
    if (nm->n == 0)
        return -1;
    unsigned start = rng_below(r, nm->n);
    unsigned fbase = nm->fdepth ? nm->fbase[nm->nfbase - 1] : 0;

    for (unsigned k = 0; k < nm->n; k++) {
        unsigned i = (start + k) % nm->n;

        if (!(kind_mask & (1u << nm->v[i].kind)))
            continue;
        if (nm->fdepth && !nm->v[i].global && i < fbase)
            continue;
        return (int)i;
    }
    return -1;
}

static void pool_add(gnames *nm, const char *s, unsigned kind,
                     unsigned global)
{
    if (nm->n >= 48)
        return;
    strcpy(nm->v[nm->n].s, s);
    nm->v[nm->n].kind = (uint8_t)kind;
    nm->v[nm->n].global = (uint8_t)global;
    nm->n++;
}

/* A syllabic identifier that never collides with a keyword. */
static void make_ident(char *s, rng_t *r, int longish)
{
    unsigned syl = 1 + rng_below(r, longish ? 3u : 2u);
    size_t k = 0;

    for (unsigned i = 0; i < syl; i++) {
        s[k++] = GCON[rng_below(r, 15)];
        s[k++] = GVO[rng_below(r, 5)];
        if (rng_below(r, 3) == 0 && k < 20)
            s[k++] = GCON[rng_below(r, 15)];
    }
    if (rng_below(r, 2)) {
        unsigned d = 1 + rng_below(r, 99);

        if (d >= 10)
            s[k++] = (char)('0' + d / 10);
        s[k++] = (char)('0' + d % 10);
    }
    s[k] = 0;
    if (keyword_of((const uint8_t *)s, k))
        s[k++] = '_';
    s[k] = 0;
}

static void gen_strlit(cbuf *b, rng_t *r)
{
    static const char HEXD[] = "0123456789abcdef";
    unsigned words = 1 + rng_below(r, 3);

    buf_putc(b, '"');
    for (unsigned i = 0; i < words; i++) {
        if (i)
            buf_putc(b, ' ');
        unsigned n = 2 + rng_below(r, 5);

        for (unsigned j = 0; j < n; j++) {
            unsigned m = rng_below(r, 24);

            if (m == 0) {
                buf_putn(b, "\\\\", 2);
            } else if (m == 1) {
                buf_putn(b, "\\\"", 2);
            } else if (m == 2) {
                buf_putn(b, "\\n", 2);
            } else if (m == 3) {
                buf_putn(b, "\\t", 2);
            } else if (m == 4) {
                buf_putn(b, "\\x", 2);
                buf_putc(b, (uint8_t)HEXD[rng_below(r, 16)]);
                buf_putc(b, (uint8_t)HEXD[rng_below(r, 16)]);
            } else {
                buf_putc(b, (uint8_t)(j & 1 ? GVO[rng_below(r, 5)]
                                            : GCON[rng_below(r, 15)]));
            }
        }
    }
    buf_putc(b, '"');
}

static void gen_intlit(cbuf *b, rng_t *r)
{
    if (rng_below(r, 4) == 0) {
        static const char HEXD[] = "0123456789abcdef";
        unsigned n = 2 + rng_below(r, 5);

        buf_putn(b, "0x", 2);
        for (unsigned i = 0; i < n; i++)
            buf_putc(b, (uint8_t)HEXD[rng_below(r, 16)]);
    } else {
        buf_putu(b, 1 + rng_below(r, 999999u));
    }
}

static const char *op_name(int op)
{
    switch (op) {
    case B_OR: return " or ";
    case B_AND: return " and ";
    case B_EQ: return " == ";
    case B_NE: return " ~= ";
    case B_LT: return " < ";
    case B_LE: return " <= ";
    case B_GT: return " > ";
    case B_GE: return " >= ";
    case B_CONCAT: return " .. ";
    case B_ADD: return " + ";
    case B_SUB: return " - ";
    case B_MUL: return " * ";
    case B_DIV: return " / ";
    default: return " % ";
    }
}

static int pick_binop(rng_t *r, unsigned arch)
{
    switch (arch) {
    case 2: { /* arithmetic kernel */
        static const uint8_t O[] = { B_ADD, B_SUB, B_MUL, B_DIV, B_MOD };

        return O[rng_below(r, 5)];
    }
    case 3: { /* string handler */
        static const uint8_t O[] = { B_CONCAT, B_CONCAT, B_EQ, B_NE,
                                     B_LT };

        return O[rng_below(r, 5)];
    }
    case 4: { /* control flow */
        static const uint8_t O[] = { B_LT, B_LE, B_GT, B_GE, B_EQ,
                                     B_NE, B_AND, B_OR };

        return O[rng_below(r, 8)];
    }
    case 1: {
        static const uint8_t O[] = { B_EQ, B_LT, B_AND, B_ADD };

        return O[rng_below(r, 4)];
    }
    default: { /* 0 and 5: mixed */
        static const uint8_t O[] = { B_EQ, B_LT, B_LE, B_AND, B_OR,
                                     B_ADD, B_SUB, B_CONCAT };

        return O[rng_below(r, 8)];
    }
    }
}

static void gen_expr(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch);

static void gen_stmt(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch,
                     int top_level);

static void gen_cond(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    if (depth >= 1 && rng_below(r, 100) < 40) {
        gen_cond(b, r, nm, depth - 1, arch);
        buf_puts(b, rng_below(r, 2) ? " and " : " or ");
        gen_cond(b, r, nm, depth - 1, arch);
        return;
    }
    gen_expr(b, r, nm, depth, arch);
}

static void gen_field(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    int i = pool_pick(r, nm, 1u << 3);

    if (i < 0)
        i = 1; /* g_tab is always seeded */
    buf_puts(b, nm->v[i].s);
    if (depth > 0 && rng_below(r, 4) == 0) {
        buf_putc(b, '.');
        buf_puts(b, GFIELDS[rng_below(r, 8)]);
        if (rng_below(r, 2)) {
            buf_putc(b, '.');
            buf_puts(b, GFIELDS[rng_below(r, 8)]);
        } else {
            buf_putc(b, '[');
            gen_expr(b, r, nm, depth - 1, arch);
            buf_putc(b, ']');
        }
        return;
    }
    if (rng_below(r, 3) == 0) {
        buf_putc(b, '[');
        gen_expr(b, r, nm, depth - 1, arch);
        buf_putc(b, ']');
    } else {
        buf_putc(b, '.');
        buf_puts(b, GFIELDS[rng_below(r, 8)]);
    }
}

static void gen_call(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    if (depth > 0 && rng_below(r, 8) == 0) {
        int t = pool_pick(r, nm, 1u << 3);

        if (t < 0)
            t = 1;
        buf_puts(b, nm->v[t].s);
        buf_putc(b, '.');
        buf_puts(b, GFIELDS[rng_below(r, 8)]);
    } else {
        int i = pool_pick(r, nm, 1u << 2);

        buf_puts(b, i < 0 ? "print" : nm->v[i].s);
    }
    buf_putc(b, '(');
    unsigned args = rng_below(r, 4);

    for (unsigned a = 0; a < args; a++) {
        if (a)
            buf_putn(b, ", ", 2);
        gen_expr(b, r, nm, depth - 1, arch);
    }
    buf_putc(b, ')');
}

static void gen_tablector(cbuf *b, rng_t *r, gnames *nm, int depth,
                          unsigned arch)
{
    buf_putc(b, '{');
    unsigned items = 1 + rng_below(r, depth > 1 ? 5u : 3u);

    for (unsigned i = 0; i < items; i++) {
        if (i)
            buf_putn(b, ", ", 2);
        unsigned m = rng_below(r, 10);

        if (depth > 0 && m < 2) {
            buf_puts(b, GFIELDS[rng_below(r, 8)]);
            buf_putn(b, " = ", 3);
            gen_expr(b, r, nm, depth - 1, arch);
        } else if (depth > 0 && m < 3) {
            buf_putc(b, '[');
            gen_expr(b, r, nm, depth - 1, arch);
            buf_putn(b, "] = ", 4);
            gen_expr(b, r, nm, depth - 1, arch);
        } else if (depth > 1 && m < 5) {
            gen_tablector(b, r, nm, depth - 1, arch);
        } else {
            gen_expr(b, r, nm, depth - 1, arch);
        }
    }
    buf_putc(b, '}');
}

static void gen_anonfunc(cbuf *b, rng_t *r, gnames *nm, int depth,
                         unsigned arch)
{
    buf_puts(b, "function(");
    pool_fenter(nm);
    unsigned np = rng_below(r, 3);

    for (unsigned p = 0; p < np; p++) {
        if (p)
            buf_putn(b, ", ", 2);
        char s[28];

        make_ident(s, r, 0);
        buf_puts(b, s);
        pool_add(nm, s, 1, 0);
    }
    buf_putc(b, ')');
    {
        unsigned n = CL_FUNC_LO + rng_below(r, CL_FUNC_RN);

        buf_putc(b, '\n');
        for (unsigned i = 0; i < n; i++)
            gen_stmt(b, r, nm, depth, arch, 0);
        if (rng_below(r, 100) < 35) {
            buf_puts(b, "return ");
            if (rng_below(r, 100) < 70)
                gen_expr(b, r, nm, depth, arch);
            buf_putc(b, '\n');
        }
    }
    pool_fleave(nm);
    buf_puts(b, "end");
}

static void gen_binop(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    int op = pick_binop(r, arch);
    unsigned n = 1 + rng_below(r, 3);

    gen_expr(b, r, nm, depth - 1, arch);
    for (unsigned i = 0; i < n; i++) {
        buf_puts(b, op_name(op));
        gen_expr(b, r, nm, depth - 1, arch);
    }
}

static void gen_expr(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    /* leaf weights per archetype: name, int, string, bool/nil */
    static const uint8_t LEAF[6][4] = {
        { 32, 20, 5, 20 },
        { 36, 12, 8, 22 },
        { 26, 34, 2, 16 },
        { 20, 6, 48, 6 },
        { 30, 18, 5, 25 },
        { 28, 17, 11, 22 },
    };
    /* compound weights per archetype:
       binop, call, field, table, anonfunc, paren/unary */
    static const uint8_t COMP[6][6] = {
        { 40, 26, 8, 4, 0, 22 },
        { 26, 14, 30, 18, 0, 12 },
        { 48, 8, 10, 6, 0, 28 },
        { 44, 12, 6, 4, 0, 34 },
        { 34, 16, 10, 6, 0, 34 },
        { 34, 18, 12, 10, 6, 20 },
    };
    unsigned m;

    if (depth < 0)
        depth = 0;
    m = rng_below(r, 100);
    if (m < LEAF[arch][0]) {
        int i = pool_pick(r, nm, (1u << 1) | (1u << 3));

        buf_puts(b, i < 0 ? "g_count" : nm->v[i].s);
        return;
    }
    m -= LEAF[arch][0];
    if (m < LEAF[arch][1]) {
        gen_intlit(b, r);
        return;
    }
    m -= LEAF[arch][1];
    if (m < LEAF[arch][2]) {
        gen_strlit(b, r);
        return;
    }
    m -= LEAF[arch][2];
    if (m < LEAF[arch][3]) {
        static const char *const BOOLS[] = { "true", "false", "nil" };

        buf_puts(b, BOOLS[rng_below(r, 3)]);
        return;
    }
    if (depth == 0) { /* no room to nest: leaf fallback */
        gen_intlit(b, r);
        return;
    }
    m = rng_below(r, 100);
    if (m < COMP[arch][0]) {
        gen_binop(b, r, nm, depth, arch);
    } else if ((unsigned)m < (unsigned)(COMP[arch][0] + COMP[arch][1])) {
        gen_call(b, r, nm, depth - 1, arch);
    } else if ((unsigned)m < (unsigned)(COMP[arch][0] + COMP[arch][1] +
                                        COMP[arch][2])) {
        gen_field(b, r, nm, depth - 1, arch);
    } else if ((unsigned)m < (unsigned)(COMP[arch][0] + COMP[arch][1] +
                                        COMP[arch][2] + COMP[arch][3])) {
        gen_tablector(b, r, nm, depth - 1, arch);
    } else if ((unsigned)m < (unsigned)(COMP[arch][0] + COMP[arch][1] +
                                        COMP[arch][2] + COMP[arch][3] +
                                        COMP[arch][4]) && arch == 5 &&
               nm->fdepth == 0) {
        gen_anonfunc(b, r, nm, depth - 1, arch);
    } else if (rng_below(r, 3)) {
        buf_putc(b, '(');
        gen_expr(b, r, nm, depth - 1, arch);
        buf_putc(b, ')');
    } else {
        switch (arch) {
        case 2:
            buf_puts(b, rng_below(r, 2) ? "- " : "# ");
            break;
        case 1: case 3:
            buf_puts(b, "- ");
            break;
        default:
            buf_puts(b, "not ");
            break;
        }
        gen_expr(b, r, nm, depth - 1, arch);
    }
}

static void gen_stmt(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch,
                     int top_level);

static void gen_block(cbuf *b, rng_t *r, gnames *nm, int depth,
                      unsigned arch)
{
    pool_enter(nm);
    unsigned n = 1 + rng_below(r, 3);

    buf_putc(b, '\n');
    for (unsigned i = 0; i < n; i++)
        gen_stmt(b, r, nm, depth, arch, 0);
    pool_leave(nm);
}

static void gen_loop_body(cbuf *b, rng_t *r, gnames *nm, int depth,
                          unsigned arch)
{
    pool_enter(nm);
    unsigned n = 1 + rng_below(r, 4);
    int brk = rng_below(r, 100) < 40;

    buf_putc(b, '\n');
    for (unsigned i = 0; i < n; i++) {
        if (brk && i == n - 1) {
            if (rng_below(r, 2)) {
                buf_puts(b, "break\n");
            } else {
                buf_puts(b, "if ");
                gen_expr(b, r, nm, depth, arch);
                buf_puts(b, " then break end\n");
            }
        } else {
            gen_stmt(b, r, nm, depth, arch, 0);
        }
    }
    pool_leave(nm);
}

static void gen_funcbody(gnames *nm, cbuf *b, rng_t *r, int depth,
                         unsigned arch)
{
    unsigned n = CL_FUNC_LO + rng_below(r, CL_FUNC_RN);
    int ret = rng_below(r, 100) < 35;

    buf_putc(b, '\n');
    for (unsigned i = 0; i < n; i++) {
        if (ret && i == n - 1) {
            buf_puts(b, "return ");
            if (rng_below(r, 100) < 70)
                gen_expr(b, r, nm, depth, arch);
            buf_putc(b, '\n');
        } else {
            gen_stmt(b, r, nm, depth, arch, 0);
        }
    }
}

static void stmt_local(cbuf *b, rng_t *r, gnames *nm, int depth,
                       unsigned arch)
{
    unsigned names = 1 + rng_below(r, 2);
    int tabval = rng_below(r, 100) < (arch == 1 ? 55u : 12u);

    buf_puts(b, "local ");
    for (unsigned i = 0; i < names; i++) {
        if (i)
            buf_putn(b, ", ", 2);
        char s[28];

        make_ident(s, r, arch == 3);
        buf_puts(b, s);
        pool_add(nm, s, tabval && i == 0 ? 3 : 1, 0);
    }
    if (rng_below(r, 100) < 85) {
        buf_puts(b, " = ");
        for (unsigned i = 0; i < names; i++) {
            if (i)
                buf_putn(b, ", ", 2);
            if (tabval && i == 0) {
                gen_tablector(b, r, nm, depth, arch);
            } else if (arch == 5 && depth >= 1 && nm->fdepth == 0 &&
                       rng_below(r, 100) < 8) {
                gen_anonfunc(b, r, nm, depth - 1, arch);
            } else {
                gen_expr(b, r, nm, depth, arch);
            }
        }
    }
    buf_putc(b, '\n');
}

static void stmt_assign(cbuf *b, rng_t *r, gnames *nm, int depth,
                        unsigned arch)
{
    unsigned targets = 1 + rng_below(r, 2);

    for (unsigned i = 0; i < targets; i++) {
        if (i)
            buf_putn(b, ", ", 2);
        int ti = pool_pick(r, nm, (1u << 1) | (1u << 3));

        if (ti < 0) {
            buf_puts(b, "g_count");
        } else if (nm->v[ti].kind == 3 && rng_below(r, 100) < 60) {
            buf_puts(b, nm->v[ti].s);
            if (rng_below(r, 3) == 0) {
                buf_putc(b, '[');
                gen_expr(b, r, nm, depth, arch);
                buf_putc(b, ']');
            } else {
                buf_putc(b, '.');
                buf_puts(b, GFIELDS[rng_below(r, 8)]);
            }
        } else {
            buf_puts(b, nm->v[ti].s);
        }
    }
    buf_puts(b, " = ");
    for (unsigned i = 0; i < targets; i++) {
        if (i)
            buf_putn(b, ", ", 2);
        gen_expr(b, r, nm, depth, arch);
    }
    buf_putc(b, '\n');
}

static void stmt_call(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    gen_call(b, r, nm, depth, arch);
    buf_putc(b, '\n');
}

static void stmt_if(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    buf_puts(b, "if ");
    gen_cond(b, r, nm, depth, arch);
    buf_puts(b, " then");
    gen_block(b, r, nm, depth - 1, arch);
    unsigned elseifs = rng_below(r, 3);

    for (unsigned e = 0; e < elseifs; e++) {
        buf_puts(b, "elseif ");
        gen_cond(b, r, nm, depth, arch);
        buf_puts(b, " then");
        gen_block(b, r, nm, depth - 1, arch);
    }
    if (rng_below(r, 100) < 45) {
        buf_puts(b, "else");
        gen_block(b, r, nm, depth - 1, arch);
    }
    buf_puts(b, "end\n");
}

static void stmt_while(cbuf *b, rng_t *r, gnames *nm, int depth,
                       unsigned arch)
{
    buf_puts(b, "while ");
    gen_cond(b, r, nm, depth, arch);
    buf_puts(b, " do");
    gen_loop_body(b, r, nm, depth - 1, arch);
    buf_puts(b, "end\n");
}

static void stmt_repeat(cbuf *b, rng_t *r, gnames *nm, int depth,
                        unsigned arch)
{
    buf_puts(b, "repeat");
    gen_loop_body(b, r, nm, depth - 1, arch);
    buf_puts(b, "until ");
    gen_cond(b, r, nm, depth, arch);
    buf_putc(b, '\n');
}

static void stmt_for(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch)
{
    char s[28];

    buf_puts(b, "for ");
    make_ident(s, r, 0);
    buf_puts(b, s);
    buf_puts(b, " = ");
    gen_intlit(b, r);
    buf_putn(b, ", ", 2);
    if (rng_below(r, 2))
        gen_intlit(b, r);
    else
        gen_expr(b, r, nm, depth, arch);
    if (rng_below(r, 100) < 40) {
        buf_putn(b, ", ", 2);
        gen_intlit(b, r);
    }
    buf_puts(b, " do");
    pool_enter(nm);
    pool_add(nm, s, 1, 0);
    gen_loop_body(b, r, nm, depth - 1, arch);
    pool_leave(nm);
    buf_puts(b, "end\n");
}

static void stmt_localfunc(cbuf *b, rng_t *r, gnames *nm, int depth,
                           unsigned arch)
{
    char s[28];

    buf_puts(b, "local function ");
    make_ident(s, r, 0);
    buf_puts(b, s);
    buf_putc(b, '(');
    pool_fenter(nm);
    unsigned np = rng_below(r, 3);

    for (unsigned p = 0; p < np; p++) {
        if (p)
            buf_putn(b, ", ", 2);
        char ps[28];

        make_ident(ps, r, 0);
        buf_puts(b, ps);
        pool_add(nm, ps, 1, 0);
    }
    buf_putc(b, ')');
    gen_funcbody(nm, b, r, depth, arch);
    pool_fleave(nm);
    pool_add(nm, s, 2, 0);
    buf_puts(b, "end\n");
}

static void stmt_globalfunc(cbuf *b, rng_t *r, gnames *nm, int depth,
                            unsigned arch)
{
    char s[28];

    buf_puts(b, "function ");
    make_ident(s, r, 0);
    buf_puts(b, s);
    buf_putc(b, '(');
    pool_fenter(nm);
    unsigned np = rng_below(r, 3);

    for (unsigned p = 0; p < np; p++) {
        if (p)
            buf_putn(b, ", ", 2);
        char ps[28];

        make_ident(ps, r, 0);
        buf_puts(b, ps);
        pool_add(nm, ps, 1, 0);
    }
    buf_putc(b, ')');
    gen_funcbody(nm, b, r, depth, arch);
    pool_fleave(nm);
    pool_add(nm, s, 2, 1);
    buf_puts(b, "end\n");
}

static void gen_stmt(cbuf *b, rng_t *r, gnames *nm, int depth, unsigned arch,
                     int top_level)
{
    unsigned m = rng_below(r, 100);
    int simple = depth < 1;

    if (simple)
        m = 20 + rng_below(r, 60); /* near the cap: flat statements only */

    switch (arch) {
    case 0: /* call graph */
        if (!simple && top_level && m < 8) {
            stmt_globalfunc(b, r, nm, depth, arch);
        } else if (m < 24) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 44) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 72) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 88) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 94) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 98) {
            stmt_for(b, r, nm, depth, arch);
        } else {
            stmt_call(b, r, nm, depth, arch);
        }
        break;
    case 1: /* table builder */
        if (m < 36) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 62) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 76) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 84) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 90) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 96) {
            stmt_for(b, r, nm, depth, arch);
        } else {
            stmt_local(b, r, nm, depth, arch);
        }
        break;
    case 2: /* arithmetic kernel */
        if (m < 30) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 58) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 64) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 72) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 82) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 96) {
            stmt_for(b, r, nm, depth, arch);
        } else {
            stmt_assign(b, r, nm, depth, arch);
        }
        break;
    case 3: /* string handler */
        if (m < 40) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 62) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 74) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 86) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 92) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 97) {
            stmt_for(b, r, nm, depth, arch);
        } else {
            stmt_local(b, r, nm, depth, arch);
        }
        break;
    case 4: /* control flow */
        if (m < 20) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 40) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 50) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 72) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 84) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 92) {
            stmt_repeat(b, r, nm, depth, arch);
        } else if (!simple && m < 97) {
            stmt_for(b, r, nm, depth, arch);
        } else {
            stmt_assign(b, r, nm, depth, arch);
        }
        break;
    default: /* 5: mixed module */
        if (!simple && top_level && m < 6) {
            stmt_globalfunc(b, r, nm, depth, arch);
        } else if (m < 28) {
            stmt_local(b, r, nm, depth, arch);
        } else if (m < 46) {
            stmt_assign(b, r, nm, depth, arch);
        } else if (m < 62) {
            stmt_call(b, r, nm, depth, arch);
        } else if (!simple && m < 76) {
            stmt_if(b, r, nm, depth, arch);
        } else if (!simple && m < 82) {
            stmt_while(b, r, nm, depth, arch);
        } else if (!simple && m < 87) {
            stmt_for(b, r, nm, depth, arch);
        } else if (!simple && m < 91) {
            stmt_repeat(b, r, nm, depth, arch);
        } else if (!simple && depth >= 1 && nm->fdepth == 0 && m < 97) {
            stmt_localfunc(b, r, nm, depth, arch);
        } else {
            stmt_call(b, r, nm, depth, arch);
        }
        break;
    }
}

static void gen_unit(cbuf *b, rng_t *r, unsigned unit, unsigned seed)
{
    static const uint8_t AF_LO[6] = { 9, 3, 3, 3, 5, 5 };
    static const uint8_t AF_RN[6] = { 4, 2, 2, 2, 3, 4 };
    unsigned arch = unit % 6;
    gnames nm;

    pool_seed(&nm);
    buf_puts(b, "-- lotofbench translation unit ");
    buf_putu(b, unit);
    buf_puts(b, " (archetype ");
    buf_putu(b, arch);
    buf_puts(b, ", seed ");
    buf_putu(b, seed);
    buf_puts(b, ")\n");
    unsigned nf = AF_LO[arch] + rng_below(r, AF_RN[arch]);

    for (unsigned i = 0; i < nf; i++) {
        if (arch == 0 || (arch == 5 && rng_below(r, 2)))
            stmt_globalfunc(b, r, &nm, 2, arch);
        else
            stmt_localfunc(b, r, &nm, 2, arch);
    }
    unsigned top = CL_TOP_LO + rng_below(r, CL_TOP_RN);

    for (unsigned i = 0; i < top; i++)
        gen_stmt(b, r, &nm, 2, arch, 1);
}

/* --- context, reference compile, run --------------------------------------------------*/

struct cl_ctx {
    unsigned seed;
    struct {
        uint8_t *src;
        size_t len;
        uint64_t tokens, nodes;
        uint8_t *blob;
        size_t blob_len;
        uint8_t digest[20];
    } unit[CLANG_UNITS];
    uint64_t tokens_total;
    cl_scratch tool;
};

cl_ctx *cl_prepare(unsigned seed)
{
    cl_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;

    if (!ctx)
        return NULL;
    ctx->seed = seed;
    rng_init(&rng, (uint64_t)seed * 0x9E3779B97F4A7C15ull +
                       0x165667B19E3779F9ull);
    for (unsigned u = 0; u < CLANG_UNITS; u++) {
        cbuf b = { NULL, 0, 0, 0 };

        gen_unit(&b, &rng, u, seed);
        if (b.oom || !b.p) {
            free(b.p);
            cl_free(ctx);
            return NULL;
        }
        ctx->unit[u].src = b.p;
        ctx->unit[u].len = b.n;
        if (cl_compile_scratch(&ctx->tool, b.p, b.n)) {
            cl_free(ctx);
            return NULL;
        }
        ctx->unit[u].tokens = ctx->tool.toks_n;
        ctx->unit[u].nodes = ctx->tool.ast_n;
        ctx->unit[u].blob_len = ctx->tool.frames[0].ser.n;
        ctx->unit[u].blob = malloc(ctx->unit[u].blob_len);
        if (!ctx->unit[u].blob) {
            cl_free(ctx);
            return NULL;
        }
        memcpy(ctx->unit[u].blob, ctx->tool.frames[0].ser.p,
               ctx->unit[u].blob_len);
        sha1(ctx->unit[u].blob, ctx->unit[u].blob_len,
             ctx->unit[u].digest);
        ctx->tokens_total += ctx->unit[u].tokens;
    }
    return ctx;
}

void cl_free(cl_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned u = 0; u < CLANG_UNITS; u++) {
        free(ctx->unit[u].src);
        free(ctx->unit[u].blob);
    }
    scratch_free(&ctx->tool);
    free(ctx);
}

/* One thread's slice: contiguous units, own arenas — threads never
 * share writable state (shared-task harness contract). */
typedef struct {
    cl_ctx *ctx;
    cl_scratch sc;
    uint32_t begin, end;
} cl_slice;

static int cl_cycle(void *arg)
{
    cl_slice *sl = arg;

    for (uint32_t u = sl->begin; u < sl->end; u++) {
        if (cl_compile_scratch(&sl->sc, sl->ctx->unit[u].src,
                               sl->ctx->unit[u].len))
            return -1;
        if (sl->sc.toks_n != sl->ctx->unit[u].tokens ||
            sl->sc.ast_n != sl->ctx->unit[u].nodes)
            return -1;
        uint8_t dig[20];

        sha1(sl->sc.frames[0].ser.p, sl->sc.frames[0].ser.n, dig);
        if (memcmp(dig, sl->ctx->unit[u].digest, 20) != 0)
            return -1;
    }
    return 0;
}

int cl_run(cl_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_ktps, uint64_t *tokens)
{
    unsigned t_count = *threads;
    cl_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (t_count > CLANG_UNITS) /* one unit per thread at least */
        t_count = CLANG_UNITS;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        uint32_t per = CLANG_UNITS / t_count;
        unsigned extra = CLANG_UNITS % t_count;
        uint32_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            uint32_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, cl_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)ctx->tokens_total / elapsed[r] / 1000.0
                           : -1.0;
        *throughput_ktps = lb_median(rates, runs);
        *threads = t_count;
        *tokens = ctx->tokens_total;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors and test hooks -----------------------------------------------------------*/

uint32_t cl_units(const cl_ctx *ctx)
{
    return ctx ? CLANG_UNITS : 0;
}

uint64_t cl_tokens(const cl_ctx *ctx)
{
    return ctx ? ctx->tokens_total : 0;
}

uint64_t cl_unit_tokens(const cl_ctx *ctx, unsigned unit)
{
    if (!ctx || unit >= CLANG_UNITS)
        return 0;
    return ctx->unit[unit].tokens;
}

uint64_t cl_unit_nodes(const cl_ctx *ctx, unsigned unit)
{
    if (!ctx || unit >= CLANG_UNITS)
        return 0;
    return ctx->unit[unit].nodes;
}

const char *cl_unit_source(const cl_ctx *ctx, unsigned unit)
{
    if (!ctx || unit >= CLANG_UNITS)
        return NULL;
    return (const char *)ctx->unit[unit].src;
}

size_t cl_unit_source_len(const cl_ctx *ctx, unsigned unit)
{
    if (!ctx || unit >= CLANG_UNITS)
        return 0;
    return ctx->unit[unit].len;
}

const uint8_t *cl_unit_blob(const cl_ctx *ctx, unsigned unit, size_t *len)
{
    if (!ctx || unit >= CLANG_UNITS) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = ctx->unit[unit].blob_len;
    return ctx->unit[unit].blob;
}

uint64_t cl_unit_digest(const cl_ctx *ctx, unsigned unit)
{
    if (!ctx || unit >= CLANG_UNITS)
        return 0;
    uint64_t v = 0;

    for (int i = 0; i < 8; i++)
        v |= (uint64_t)ctx->unit[unit].digest[i] << (8 * i);
    return v;
}

long cl_lex_count(cl_ctx *ctx, const char *src, size_t len)
{
    if (!ctx || !src)
        return -1;
    scratch_reset(&ctx->tool);
    if (lex_chunk(&ctx->tool, (const uint8_t *)src, len))
        return -1;
    return (long)ctx->tool.toks_n;
}

long cl_parse_count(cl_ctx *ctx, const char *src, size_t len)
{
    if (!ctx || !src)
        return -1;
    scratch_reset(&ctx->tool);
    if (lex_chunk(&ctx->tool, (const uint8_t *)src, len))
        return -1;
    ctx->tool.cur = 0;
    ctx->tool.root = p_block(&ctx->tool);
    if (ctx->tool.err || ctx->tool.cur != ctx->tool.toks_n)
        return -1;
    return (long)ctx->tool.ast_n;
}

long cl_compile(cl_ctx *ctx, const char *src, size_t len,
                uint8_t *out, size_t cap)
{
    if (!ctx || !src)
        return -1;
    if (cl_compile_scratch(&ctx->tool, (const uint8_t *)src, len))
        return -1;
    size_t n = ctx->tool.frames[0].ser.n;

    if (!out || n > cap)
        return -1;
    memcpy(out, ctx->tool.frames[0].ser.p, n);
    return (long)n;
}
