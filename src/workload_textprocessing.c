/* LotofBench — Text Processing workload (workload #7).
 *
 * Mirrors the seventh Geekbench 6 CPU workload: regex matching and
 * Markdown/HTML processing over a text corpus — Geekbench 6 runs it in
 * Python 3. A scripted runtime cannot be part of a reproducible
 * benchmark (interpreter versions, library drift), so lotofbench
 * reproduces the shape of that pipeline dependency-free, with the same
 * honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of 32 Markdown
 *     documents in six archetype families (API documentation, blog
 *     post, changelog, log digest, FAQ, README) that cycle the way a
 *     real site's pages do. Fixed --seed means byte-identical corpora
 *     on every machine, so scores are comparable across runs. The
 *     documents are generated off the clock — the stand-in for the
 *     network fetch or the checkout;
 *   - a hand-rolled regex engine — every document is scanned by its
 *     own set of 8 seeded patterns (literals, `.`, character classes,
 *     the \d \w \s shorthands, groups, alternation, greedy * + ? {m,n}
 *     on single-character atoms, ^ $ anchors), compiled once to a
 *     bytecode run by a bounded backtracking VM. The subset forbids
 *     quantified groups, so no pattern can backtrack catastrophically;
 *     a pathological pattern trips a step budget and fails the run
 *     instead of hanging;
 *   - a from-scratch Markdown-to-HTML converter — headings, fenced
 *     code, block quotes, ordered and unordered lists, horizontal
 *     rules, paragraphs with soft-break-to-space, and the inline
 *     elements (code spans, links, emphasis, HTML escaping) —
 *     every document converted from scratch inside the timed cycle;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     pattern reproduces its reference match count and every converted
 *     document reproduces its reference SHA-1 digest; any divergence
 *     aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the document
 *     set is split into equal contiguous slices, one per thread.
 *
 * The throughput metric is bytes of text processed: every document is
 * counted once for the Markdown stage and once per pattern for the
 * regex stage (each pattern scans the whole document). The task size
 * is exact and machine-independent, like the corpus size of workload
 * #1.
 *
 * Deliberate divergences from a real regex/Markdown stack (all
 * documented in docs/benchmarks.md): a safe regex subset (no
 * quantified groups, no captures, no backreferences, no lookahead, no
 * multiline mode), a Markdown subset (no nested block structures, no
 * reference links, no setext headings, single-level quotes), and C
 * byte processing instead of a Python interpreter. */

#include "workload_textprocessing.h"

#include "harness.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

/* --- growable byte buffer ------------------------------------------------*/

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

static void buf_reset(cbuf *b)
{
    b->n = 0;
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

/* --- input-size knobs ------------------------------------------------------*/

/* Per-document shape. The corpus lands around 1.5 MB (about 46 KB per
 * document); a single-core pass — 8 regex scans + one Markdown
 * conversion per document — takes 100-200 ms on the reference
 * machine. */
#define TP_SECTIONS_LO 30u /* sections per document (min) */
#define TP_SECTIONS_RN 10u /* ... + rng_below(range) */
#define TP_PARAS_LO    3u  /* paragraphs per section (min) */
#define TP_PARAS_RN    3u  /* ... + rng_below(range) */
#define TP_SENTS_LO    4u  /* sentences per paragraph (min) */
#define TP_SENTS_RN    4u  /* ... + rng_below(range) */
#define TP_WORDS_LO    6u  /* words per sentence (min) */
#define TP_WORDS_RN    9u  /* ... + rng_below(range) */
#define TP_LIST_LO     3u  /* items per list (min) */
#define TP_LIST_RN     4u  /* ... + rng_below(range) */
#define TP_FENCE_LO    4u  /* lines per fenced block (min) */
#define TP_FENCE_RN    4u  /* ... + rng_below(range) */
#define TP_WORDS       40u /* vocabulary per document */

/* --- the regex engine --------------------------------------------------------
 *
 * Patterns compile to a flat bytecode run by a backtracking VM with an
 * explicit stack. Instructions are 4 bytes: opcode, `a`, `b`. SPLIT
 * tries the fall-through path first (greedy, leftmost-first
 * semantics), pushing the alternative. The step budget turns a
 * pathological pattern (impossible in the safe subset) into a
 * verified failure instead of a hang. */

#define TP_PAT_MAX   96u    /* pattern source length cap */
#define TP_PROG_MAX  512u   /* bytecode instruction cap */
#define TP_CLASS_MAX 16u    /* distinct character classes per pattern */
#define TP_DEPTH_MAX 32u    /* group nesting cap */
#define TP_REP_MAX   8u     /* {m,n} bound cap */

enum {
    R_CHAR = 1, /* match literal byte a */
    R_ANY,      /* match any byte except '\n' */
    R_CLASS,    /* match class bitmap a (negated when b & 1) */
    R_BOL,      /* assert start of text */
    R_EOL,      /* assert end of text */
    R_JMP,      /* jump to b */
    R_SPLIT,    /* try fall-through first, push alternative b */
    R_MATCH,
    R_FAIL      /* alternation exhausted: fail, never an empty match */
};

typedef struct {
    uint8_t  op;
    uint8_t  a;
    uint16_t b;
} re_ins;

typedef struct {
    re_ins   ins[TP_PROG_MAX];
    unsigned n;
    uint8_t  cls[TP_CLASS_MAX][32];
    unsigned ncls;
    int      err;
} re_prog;

static void emit(re_prog *p, uint8_t op, uint8_t a, uint16_t b)
{
    if (p->err)
        return;
    if (p->n >= TP_PROG_MAX) {
        p->err = 1;
        return;
    }
    p->ins[p->n].op = op;
    p->ins[p->n].a = a;
    p->ins[p->n].b = b;
    p->n++;
}

static unsigned here(const re_prog *p)
{
    return p->n;
}

static void patch(re_prog *p, unsigned at, uint16_t target)
{
    if (!p->err)
        p->ins[at].b = target;
}

static void class_set(re_prog *p, uint8_t c)
{
    if (p->err)
        return;
    p->cls[p->ncls][c >> 3] |= (uint8_t)(1u << (c & 7));
}

/* Add one item (range, char, shorthand) to the class under construction. */
static void class_item(re_prog *p, const uint8_t *s, size_t n, size_t *i)
{
    uint8_t lo, hi;

    if (s[*i] == '\\') { /* shorthand or escaped char inside a class */
        if (*i + 1 >= n) {
            p->err = 1;
            return;
        }
        uint8_t e = s[*i + 1];

        *i += 2;
        if (e == 'd') {
            for (uint8_t c = '0'; c <= '9'; c++)
                class_set(p, c);
            return;
        }
        if (e == 'w') {
            for (uint8_t c = 'a'; c <= 'z'; c++)
                class_set(p, c);
            for (uint8_t c = 'A'; c <= 'Z'; c++)
                class_set(p, c);
            for (uint8_t c = '0'; c <= '9'; c++)
                class_set(p, c);
            class_set(p, '_');
            return;
        }
        if (e == 's') {
            class_set(p, ' ');
            class_set(p, '\t');
            class_set(p, '\n');
            class_set(p, '\r');
            class_set(p, 0x0b); /* \v */
            class_set(p, 0x0c); /* \f */
            return;
        }
        if (e == 'n') {
            class_set(p, '\n');
            return;
        }
        if (e == 't') {
            class_set(p, '\t');
            return;
        }
        if (('a' <= e && e <= 'z') || ('A' <= e && e <= 'Z') ||
            ('0' <= e && e <= '9')) {
            p->err = 1; /* \q, \1... have no class meaning */
            return;
        }
        class_set(p, e); /* \] \[ \\ \^ \- and any other punctuation */
        return;
    }
    lo = s[*i];
    if (*i + 2 < n && s[*i + 1] == '-' && s[*i + 2] != ']') {
        hi = s[*i + 2];
        *i += 3;
    } else {
        hi = lo;
        *i += 1;
    }
    if (hi < lo) {
        p->err = 1;
        return;
    }
    for (uint16_t c = lo; c <= hi; c++)
        class_set(p, (uint8_t)c);
}

/* Parse a [...] class, emit its instruction, return 1. `neg` receives
 * the negation flag. */
static void parse_class(re_prog *p, const uint8_t *s, size_t n, size_t *i)
{
    int neg = 0;

    *i += 1; /* the '[' */
    if (p->ncls >= TP_CLASS_MAX) {
        p->err = 1;
        return;
    }
    unsigned ci = p->ncls;
    unsigned want = ci;

    memset(p->cls[want], 0, 32);
    if (*i < n && s[*i] == '^') {
        neg = 1;
        *i += 1;
    }
    if (*i < n && s[*i] == ']') { /* leading ] is literal */
        class_set(p, ']');
        *i += 1;
    }
    while (*i < n && s[*i] != ']')
        class_item(p, s, n, i);
    if (*i >= n) { /* unterminated class */
        p->err = 1;
        return;
    }
    *i += 1; /* the ']' */
    if (p->err)
        return;
    /* the negation itself lives in the R_CLASS flag; the VM xors it
     * into the bitmap test — no compile-time inversion here */
    p->ncls = want + 1;
    emit(p, R_CLASS, (uint8_t)want, neg ? 1 : 0);
}

typedef struct {
    const uint8_t *s;
    size_t n, i;
    re_prog *out;
    unsigned depth;
} re_parser;

static void p_alt(re_parser *ps);

/* Parse one atom. Returns 1 when the atom is a quantifiable
 * single-character instruction whose code sits at *at, 0 otherwise
 * (anchors, groups, empty). */
static int p_atom(re_parser *ps, unsigned *at)
{
    re_prog *p = ps->out;
    const uint8_t *s = ps->s;
    size_t n = ps->n;

    if (ps->i >= n)
        return 0;
    uint8_t c = s[ps->i];

    switch (c) {
    case '^':
        ps->i++;
        emit(p, R_BOL, 0, 0);
        return 0;
    case '$':
        ps->i++;
        emit(p, R_EOL, 0, 0);
        return 0;
    case '(':
        ps->i++;
        if (++ps->depth > TP_DEPTH_MAX) {
            p->err = 1;
            return 0;
        }
        p_alt(ps);
        ps->depth--;
        if (ps->i >= n || s[ps->i] != ')') {
            p->err = 1;
            return 0;
        }
        ps->i++;
        return 0;
    case '[':
        parse_class(p, s, n, &ps->i);
        *at = here(p) - 1;
        return !p->err;
    case '.':
        ps->i++;
        emit(p, R_ANY, 0, 0);
        *at = here(p) - 1;
        return 1;
    case '*': case '+': case '?':
        p->err = 1; /* nothing to repeat */
        return 0;
    case '|': case ')':
        return 0; /* consumed by the caller */
    case '{': {
        /* a '{' that does not open a quantifier shape is a literal */
        size_t j = ps->i + 1;
        unsigned has_m = 0;

        while (j < n && '0' <= s[j] && s[j] <= '9') {
            has_m = 1;
            j++;
        }
        if (j < n && s[j] == ',') {
            j++;
            while (j < n && '0' <= s[j] && s[j] <= '9')
                j++;
        }
        if (j < n && s[j] == '}' && has_m) {
            /* quantifier-shaped but nothing to repeat */
            p->err = 1;
            return 0;
        }
        ps->i++;
        emit(p, R_CHAR, '{', 0);
        *at = here(p) - 1;
        return 1;
    }
    case '\\': {
        if (ps->i + 1 >= n) {
            p->err = 1;
            return 0;
        }
        uint8_t e = s[ps->i + 1];

        ps->i += 2;
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' ||
            e == 'S') {
            if (p->ncls >= TP_CLASS_MAX) {
                p->err = 1;
                return 0;
            }
            unsigned ci = p->ncls;

            memset(p->cls[ci], 0, 32);
            if (e == 'd' || e == 'D')
                for (uint8_t x = '0'; x <= '9'; x++)
                    class_set(p, x);
            if (e == 'w' || e == 'W') {
                for (uint8_t x = 'a'; x <= 'z'; x++)
                    class_set(p, x);
                for (uint8_t x = 'A'; x <= 'Z'; x++)
                    class_set(p, x);
                for (uint8_t x = '0'; x <= '9'; x++)
                    class_set(p, x);
                class_set(p, '_');
            }
            if (e == 's' || e == 'S') {
                class_set(p, ' ');
                class_set(p, '\t');
                class_set(p, '\n');
                class_set(p, '\r');
                class_set(p, 0x0b); /* \v */
                class_set(p, 0x0c); /* \f */
            }
            /* \D \W \S: the negation is carried by the R_CLASS flag,
             * not baked into the bitmap */
            p->ncls = ci + 1;
            emit(p, R_CLASS, (uint8_t)ci,
                 (uint8_t)(e == 'D' || e == 'W' || e == 'S'));
            *at = here(p) - 1;
            return 1;
        }
        if (e == 'n') {
            emit(p, R_CHAR, '\n', 0);
            *at = here(p) - 1;
            return 1;
        }
        if (e == 't') {
            emit(p, R_CHAR, '\t', 0);
            *at = here(p) - 1;
            return 1;
        }
        if (('a' <= e && e <= 'z') || ('A' <= e && e <= 'Z') ||
            ('0' <= e && e <= '9')) {
            p->err = 1; /* \q \1 and friends are not in the subset */
            return 0;
        }
        emit(p, R_CHAR, e, 0); /* \. \* \[ \( \\ and all punctuation */
        *at = here(p) - 1;
        return 1;
    }
    default:
        ps->i++;
        emit(p, R_CHAR, c, 0);
        *at = here(p) - 1;
        return 1;
    }
}

/* Apply a quantifier to the single instruction at `at` (greedy). */
static void p_quantify(re_prog *p, unsigned at, uint8_t q,
                       unsigned m, unsigned nn, int bounded)
{
    re_ins atom = p->ins[at];

    p->n = at; /* the atom is re-emitted inside the quantified form */
    switch (q) {
    case '*':
        emit(p, R_SPLIT, 0, (uint16_t)(at + 3));
        emit(p, atom.op, atom.a, atom.b);
        emit(p, R_JMP, 0, (uint16_t)at);
        break;
    case '+':
        /* X+ = X X* : the star part keeps the greedy fall-through */
        emit(p, atom.op, atom.a, atom.b);
        {
            unsigned l = here(p);

            emit(p, R_SPLIT, 0, (uint16_t)(l + 3));
            emit(p, atom.op, atom.a, atom.b);
            emit(p, R_JMP, 0, (uint16_t)l);
        }
        break;
    case '?':
        emit(p, R_SPLIT, 0, (uint16_t)(at + 2));
        emit(p, atom.op, atom.a, atom.b);
        break;
    default: { /* {m} {m,} {m,n} — already validated */
        for (unsigned k = 0; k < m; k++)
            emit(p, atom.op, atom.a, atom.b);
        if (!bounded) { /* {m,} = m copies then a star */
            unsigned top = here(p);

            emit(p, R_SPLIT, 0, (uint16_t)(top + 3));
            emit(p, atom.op, atom.a, atom.b);
            emit(p, R_JMP, 0, (uint16_t)top);
        } else if (nn > m) { /* {m,n} = m copies + (n-m) optionals */
            for (unsigned k = 0; k < nn - m; k++) {
                unsigned skip = here(p);

                emit(p, R_SPLIT, 0, (uint16_t)(skip + 2));
                emit(p, atom.op, atom.a, atom.b);
            }
        }
        break;
    }
    }
}

/* Concatenation: atoms with optional quantifiers, until '|' ')' or end. */
static void p_cat(re_parser *ps)
{
    re_prog *p = ps->out;

    while (!p->err && ps->i < ps->n && ps->s[ps->i] != '|' &&
           ps->s[ps->i] != ')') {
        unsigned at = 0;
        int single = p_atom(ps, &at);

        if (p->err)
            return;
        if (single && ps->i < ps->n) {
            uint8_t c = ps->s[ps->i];
            uint8_t q = 0;
            unsigned m = 0, nn = 0;
            int bounded = 0;

            if (c == '*' || c == '+' || c == '?') {
                q = c;
                ps->i++;
            } else if (c == '{') {
                size_t j = ps->i + 1;
                unsigned mv = 0, has_m = 0, nv = 0, has_n = 0, comma = 0;

                while (j < ps->n && '0' <= ps->s[j] && ps->s[j] <= '9') {
                    mv = mv * 10 + (unsigned)(ps->s[j] - '0');
                    has_m = 1;
                    j++;
                }
                if (j < ps->n && ps->s[j] == ',') {
                    comma = 1;
                    j++;
                    while (j < ps->n && '0' <= ps->s[j] &&
                           ps->s[j] <= '9') {
                        nv = nv * 10 + (unsigned)(ps->s[j] - '0');
                        has_n = 1;
                        j++;
                    }
                }
                if (j < ps->n && ps->s[j] == '}' && has_m &&
                    mv <= TP_REP_MAX &&
                    (!comma || (has_n ? (nv <= TP_REP_MAX && mv <= nv)
                                      : 1))) {
                    q = '{';
                    /* {m} is exact (no extra), {m,} is open-ended
                     * (star tail), {m,n} is bounded (optionals) */
                    bounded = comma ? has_n : 1;
                    m = mv;
                    nn = has_n ? nv : mv;
                    ps->i = j + 1;
                }
                /* else: a literal '{' — the atom already consumed it */
            }
            if (q)
                p_quantify(p, at, q, m, nn, bounded);
        }
    }
}

/* Alternation: cat ('|' cat)*, with the SPLIT/JMP scaffolding. Each
 * branch is entered through a chooser SPLIT: fall-through is the
 * branch itself, the pushed alternative is the next chooser, so a
 * failed branch backtracks into the following one. When every chooser
 * is exhausted the path dies on R_FAIL — a failed alternation never
 * degrades into a silent empty match. */
static void p_alt(re_parser *ps)
{
    re_prog *p = ps->out;
    unsigned jmps[TP_DEPTH_MAX * 8];
    unsigned splits[TP_DEPTH_MAX * 8];
    unsigned nj = 0, ns = 0;
    unsigned fail_at, end;

    splits[ns++] = here(p);
    emit(p, R_SPLIT, 0, 0); /* chooser for branch 1 */
    p_cat(ps);
    while (!p->err && ps->i < ps->n && ps->s[ps->i] == '|') {
        ps->i++;
        if (nj >= sizeof jmps / sizeof jmps[0] ||
            ns >= sizeof splits / sizeof splits[0]) {
            p->err = 1;
            return;
        }
        jmps[nj++] = here(p);
        emit(p, R_JMP, 0, 0); /* branch taken: skip the other branches */
        splits[ns++] = here(p);
        emit(p, R_SPLIT, 0, 0); /* chooser for the next branch */
        p_cat(ps);
    }
    if (p->err)
        return;
    jmps[nj++] = here(p);
    emit(p, R_JMP, 0, 0); /* the last branch is done: skip R_FAIL */
    fail_at = here(p);
    emit(p, R_FAIL, 0, 0);
    end = here(p); /* re_compile puts R_MATCH here */
    for (unsigned k = 0; k < nj; k++)
        patch(p, jmps[k], (uint16_t)end);
    for (unsigned k = 0; k < ns - 1; k++)
        patch(p, splits[k], (uint16_t)splits[k + 1]);
    patch(p, splits[ns - 1], (uint16_t)fail_at);
}

static int re_compile(re_prog *p, const char *pattern, size_t len)
{
    re_parser ps;

    memset(p, 0, sizeof(*p));
    if (len > TP_PAT_MAX) {
        p->err = 1;
        return -1;
    }
    ps.s = (const uint8_t *)pattern;
    ps.n = len;
    ps.i = 0;
    ps.out = p;
    ps.depth = 0;
    p_alt(&ps);
    if (p->err)
        return -1;
    if (ps.i != ps.n) { /* unbalanced ')' */
        p->err = 1;
        return -1;
    }
    emit(p, R_MATCH, 0, 0);
    return p->err ? -1 : 0;
}

/* --- the backtracking VM ------------------------------------------------------*/

typedef struct {
    uint32_t pc;
    size_t sp;
} vm_frame;

typedef struct {
    vm_frame *stk;
    size_t cap, n;
    uint64_t steps;
    int exhausted;
} re_machine;

static void machine_free(re_machine *m)
{
    free(m->stk);
    m->stk = NULL;
    m->cap = m->n = 0;
}

#define TP_STACK_MAX (1u << 20) /* backtrack-frames cap (16 MB worst) */

static int vm_push(re_machine *m, uint32_t pc, size_t sp)
{
    if (m->n == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        vm_frame *ns;

        if (nc > TP_STACK_MAX)
            return -1;
        ns = realloc(m->stk, nc * sizeof(*ns));
        if (!ns)
            return -1;
        m->stk = ns;
        m->cap = nc;
    }
    m->stk[m->n].pc = pc;
    m->stk[m->n].sp = sp;
    m->n++;
    return 0;
}

/* One leftmost search anchored at `start`. Returns 1 with *end_out at
 * the match end, 0 on no match (or on a tripped budget: the caller
 * checks m->exhausted). */
static int vm_run(const re_prog *p, re_machine *m, const uint8_t *t,
                  size_t len, size_t start, size_t *end_out)
{
    uint64_t budget = 1024 + (uint64_t)len * (uint64_t)p->n * 8ull;

    m->n = 0;
    m->steps = 0;
    m->exhausted = 0;
    if (vm_push(m, 0, start)) {
        m->exhausted = 1;
        return 0;
    }
    while (m->n) {
        vm_frame f = m->stk[--m->n];

        for (;;) {
            const re_ins *in;

            if (++m->steps > budget) {
                m->exhausted = 1;
                return 0;
            }
            in = &p->ins[f.pc];
            switch (in->op) {
            case R_CHAR:
                if (f.sp < len && t[f.sp] == in->a) {
                    f.pc++;
                    f.sp++;
                    continue;
                }
                break;
            case R_ANY:
                if (f.sp < len && t[f.sp] != '\n') {
                    f.pc++;
                    f.sp++;
                    continue;
                }
                break;
            case R_CLASS: {
                int bit = 0;

                if (f.sp < len)
                    bit = (int)(p->cls[in->a][t[f.sp] >> 3] >>
                                (t[f.sp] & 7)) & 1;
                if (f.sp < len && (bit ^ (int)(in->b & 1)) == 1) {
                    f.pc++;
                    f.sp++;
                    continue;
                }
                break;
            }
            case R_BOL:
                if (f.sp == 0) {
                    f.pc++;
                    continue;
                }
                break;
            case R_EOL:
                if (f.sp == len) {
                    f.pc++;
                    continue;
                }
                break;
            case R_JMP:
                f.pc = in->b;
                continue;
            case R_SPLIT:
                if (vm_push(m, in->b, f.sp)) {
                    m->exhausted = 1;
                    return 0;
                }
                f.pc++;
                continue;
            case R_FAIL:
                break; /* dead path: backtrack */
            default: /* R_MATCH */
                *end_out = f.sp;
                return 1;
            }
            break; /* dead path: backtrack */
        }
    }
    return 0;
}

/* Count non-overlapping leftmost matches over the whole text: at each
 * position take the first match (greedy, branch order); an empty match
 * counts once and the scan advances one byte. Returns -1 if the step
 * budget tripped (pattern outside the safe subset). */
static long re_count(const re_prog *p, re_machine *m, const uint8_t *t,
                     size_t len)
{
    long count = 0;
    size_t pos = 0;

    while (pos <= len) {
        size_t end = 0;

        if (vm_run(p, m, t, len, pos, &end)) {
            count++;
            pos = end > pos ? end : pos + 1;
        } else {
            if (m->exhausted)
                return -1;
            pos++;
        }
    }
    return count;
}

/* --- Markdown to HTML --------------------------------------------------------
 *
 * The documented subset (docs/benchmarks.md): ATX headings (# to
 * ###### + space), fenced code (``` blocks, verbatim and escaped),
 * block quotes ("> ", per-line content), unordered (- * +) and ordered
 * (digits + ". ") lists, horizontal rules (--- or ***), paragraphs
 * (consecutive plain lines joined with a single space). Inline: & < >
 * escaped everywhere, `code spans` (content escaped), [links](urls)
 * (URL escaped), **strong** and *em* (same-line, no empty or
 * space-padded runs). Every block emits its HTML followed by one '\n';
 * an unterminated fence runs to the end of the document, unmatched
 * inline markers stay literal. */

static void md_escape(cbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&')
            buf_puts(b, "&amp;");
        else if (s[i] == '<')
            buf_puts(b, "&lt;");
        else if (s[i] == '>')
            buf_puts(b, "&gt;");
        else
            buf_putc(b, (uint8_t)s[i]);
    }
}

static void md_escape_url(cbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&')
            buf_puts(b, "&amp;");
        else if (s[i] == '<')
            buf_puts(b, "&lt;");
        else if (s[i] == '>')
            buf_puts(b, "&gt;");
        else if (s[i] == '"')
            buf_puts(b, "&quot;");
        else
            buf_putc(b, (uint8_t)s[i]);
    }
}

static void md_inline(cbuf *b, const char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        char c = s[i];

        if (c == '&' || c == '<' || c == '>') {
            md_escape(b, s + i, 1);
            i++;
        } else if (c == '`') {
            size_t j = i + 1;

            while (j < n && s[j] != '`')
                j++;
            if (j < n && j > i + 1) {
                buf_puts(b, "<code>");
                md_escape(b, s + i + 1, j - i - 1);
                buf_puts(b, "</code>");
                i = j + 1;
            } else {
                buf_putc(b, '`');
                i++;
            }
        } else if (c == '[') {
            size_t j = i + 1, k;

            while (j < n && s[j] != ']' && s[j] != '\n')
                j++;
            if (j + 1 < n && s[j] == ']' && s[j + 1] == '(') {
                k = j + 2;
                while (k < n && s[k] != ')' && s[k] != ' ' &&
                       s[k] != '\n')
                    k++;
            } else {
                k = 0;
                j = n; /* force the literal branch */
            }
            if (j + 1 < n && k < n && s[k] == ')' && k > j + 2) {
                buf_puts(b, "<a href=\"");
                md_escape_url(b, s + j + 2, k - j - 2);
                buf_puts(b, "\">");
                md_inline(b, s + i + 1, j - i - 1);
                buf_puts(b, "</a>");
                i = k + 1;
            } else {
                buf_putc(b, '[');
                i++;
            }
        } else if (c == '*') {
            if (i + 1 < n && s[i + 1] == '*') {
                size_t k = i + 2;

                while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '*') &&
                       s[k] != '*' && s[k] != '\n')
                    k++;
                if (k + 1 < n && s[k] == '*' && s[k + 1] == '*' &&
                    k > i + 2 && s[i + 2] != ' ' && s[k - 1] != ' ') {
                    buf_puts(b, "<strong>");
                    md_inline(b, s + i + 2, k - i - 2);
                    buf_puts(b, "</strong>");
                    i = k + 2;
                } else {
                    buf_putc(b, '*');
                    i++;
                }
            } else {
                size_t k = i + 1;

                while (k < n && s[k] != '*' && s[k] != '\n')
                    k++;
                if (k < n && k > i + 1 && s[i + 1] != ' ' &&
                    s[k - 1] != ' ') {
                    buf_puts(b, "<em>");
                    md_inline(b, s + i + 1, k - i - 1);
                    buf_puts(b, "</em>");
                    i = k + 1;
                } else {
                    buf_putc(b, '*');
                    i++;
                }
            }
        } else {
            buf_putc(b, (uint8_t)c);
            i++;
        }
    }
}

static size_t line_end(const uint8_t *s, size_t len, size_t pos)
{
    size_t e = pos;

    while (e < len && s[e] != '\n')
        e++;
    return e;
}

static int is_blank(const uint8_t *s, size_t a, size_t e)
{
    (void)s;
    return a == e;
}

static int is_fence(const uint8_t *s, size_t a, size_t e, int closing)
{
    size_t n = e - a;

    if (n < 3 || s[a] != '`' || s[a + 1] != '`' || s[a + 2] != '`')
        return 0;
    if (!closing)
        return 1;
    for (size_t i = a + 3; i < e; i++)
        if (s[i] != '`')
            return 0;
    return 1;
}

static unsigned heading_level(const uint8_t *s, size_t a, size_t e)
{
    unsigned k = 1;

    if (s[a] != '#')
        return 0;
    while (k < e - a && k < 6 && s[a + k] == '#')
        k++;
    if (k <= 6 && a + k < e && s[a + k] == ' ')
        return k;
    return 0;
}

static int is_hr(const uint8_t *s, size_t a, size_t e)
{
    if (e - a < 3)
        return 0;
    if (s[a] != '-' && s[a] != '*')
        return 0;
    for (size_t i = a; i < e; i++)
        if (s[i] != s[a])
            return 0;
    return 1;
}

static int is_quote(const uint8_t *s, size_t a, size_t e)
{
    return s[a] == '>' && (a + 1 == e || s[a + 1] == ' ');
}

static int is_ul(const uint8_t *s, size_t a, size_t e)
{
    return (s[a] == '-' || s[a] == '*' || s[a] == '+') && a + 2 <= e &&
           s[a + 1] == ' ';
}

static int is_ol(const uint8_t *s, size_t a, size_t e, size_t *content)
{
    size_t i = a;

    if (s[i] < '0' || s[i] > '9')
        return 0;
    while (i < e && '0' <= s[i] && s[i] <= '9')
        i++;
    if (i - a > 9 || i + 1 >= e || s[i] != '.' || s[i + 1] != ' ')
        return 0;
    *content = i + 2;
    return 1;
}

static void md_convert(cbuf *out, const uint8_t *s, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        size_t e = line_end(s, len, pos);
        size_t next = e < len ? e + 1 : len;

        if (is_blank(s, pos, e)) {
            pos = next;
            continue;
        }
        if (is_fence(s, pos, e, 0)) { /* open (info string ignored) */
            int first = 1;

            buf_puts(out, "<pre><code>");
            pos = next;
            while (pos < len) {
                size_t e2 = line_end(s, len, pos);

                if (is_fence(s, pos, e2, 1)) {
                    pos = e2 < len ? e2 + 1 : len;
                    break;
                }
                if (!first)
                    buf_putc(out, '\n');
                first = 0;
                md_escape(out, (const char *)s + pos, e2 - pos);
                pos = e2 < len ? e2 + 1 : len;
            }
            buf_puts(out, "</code></pre>\n");
            continue;
        }
        {
            unsigned lvl = heading_level(s, pos, e);

            if (lvl) {
                buf_puts(out, "<h");
                buf_putc(out, (uint8_t)('0' + lvl));
                buf_puts(out, ">");
                md_inline(out, (const char *)s + pos + lvl + 1,
                          e - pos - lvl - 1);
                buf_puts(out, "</h");
                buf_putc(out, (uint8_t)('0' + lvl));
                buf_puts(out, ">\n");
                pos = next;
                continue;
            }
        }
        if (is_hr(s, pos, e)) {
            buf_puts(out, "<hr>\n");
            pos = next;
            continue;
        }
        if (is_quote(s, pos, e)) {
            int first = 1;

            buf_puts(out, "<blockquote>");
            while (pos < len) {
                size_t e2 = line_end(s, len, pos);
                size_t skip;

                if (!is_quote(s, pos, e2))
                    break;
                skip = pos + 1 == e2 ? 1 : 2;
                if (!first)
                    buf_putc(out, '\n');
                first = 0;
                md_inline(out, (const char *)s + pos + skip,
                          e2 - pos - skip);
                pos = e2 < len ? e2 + 1 : len;
            }
            buf_puts(out, "</blockquote>\n");
            continue;
        }
        if (is_ul(s, pos, e)) {
            buf_puts(out, "<ul>");
            while (pos < len) {
                size_t e2 = line_end(s, len, pos);

                if (!is_ul(s, pos, e2))
                    break;
                buf_puts(out, "<li>");
                md_inline(out, (const char *)s + pos + 2, e2 - pos - 2);
                buf_puts(out, "</li>");
                pos = e2 < len ? e2 + 1 : len;
            }
            buf_puts(out, "</ul>\n");
            continue;
        }
        {
            size_t content = 0;

            if (is_ol(s, pos, e, &content)) {
                buf_puts(out, "<ol>");
                while (pos < len) {
                    size_t e2 = line_end(s, len, pos);

                    if (!is_ol(s, pos, e2, &content))
                        break;
                    buf_puts(out, "<li>");
                    md_inline(out, (const char *)s + content,
                              e2 - content);
                    buf_puts(out, "</li>");
                    pos = e2 < len ? e2 + 1 : len;
                }
                buf_puts(out, "</ol>\n");
                continue;
            }
        }
        /* paragraph run: consecutive plain lines, one <p>, lines
         * processed independently and joined with a single space */
        buf_puts(out, "<p>");
        {
            size_t content = 0;
            int first = 1;

            while (pos < len) {
                size_t e2 = line_end(s, len, pos);

                if (is_blank(s, pos, e2) ||
                    is_fence(s, pos, e2, 0) || heading_level(s, pos, e2) ||
                    is_hr(s, pos, e2) || is_quote(s, pos, e2) ||
                    is_ul(s, pos, e2) || is_ol(s, pos, e2, &content))
                    break;
                if (!first)
                    buf_putc(out, ' ');
                first = 0;
                md_inline(out, (const char *)s + pos, e2 - pos);
                pos = e2 < len ? e2 + 1 : len;
            }
        }
        buf_puts(out, "</p>\n");
    }
}

/* --- synthetic corpus generator -----------------------------------------------
 *
 * Six archetype families — API documentation, blog post, changelog,
 * log digest, FAQ, README — the way a real site's pages differ.
 * Everything is seeded: same --seed, byte-identical documents on every
 * machine. Words are syllabic and purely alphabetic, so the Markdown
 * metacharacters appear only where the generator intends an inline
 * element. Each document carries its own pattern set: two or three
 * signature templates for its archetype plus generic ones from a
 * shared pool, all inside the safe subset. */

static const char TCON[] = "bcdfgklmnprstvz";
static const char TVO[] = "aeiou";

static void make_word(char *s, rng_t *r, int longish)
{
    unsigned syl = 2 + rng_below(r, longish ? 2u : 1u);
    size_t k = 0;

    for (unsigned i = 0; i < syl; i++) {
        s[k++] = TCON[rng_below(r, 15)];
        s[k++] = TVO[rng_below(r, 5)];
        if (rng_below(r, 4) == 0 && k < 14)
            s[k++] = TCON[rng_below(r, 15)];
    }
    s[k] = 0;
}

static void make_capword(char *s, rng_t *r)
{
    make_word(s + 1, r, 0);
    s[0] = (char)((unsigned char)s[1] - 'a' + 'A');
}

/* Pattern templates. Every emitted pattern stays inside the safe
 * subset; the generator only picks from this fixed menu. */
enum {
    PT_WORD = 0,   /* literal word */
    PT_CAP,        /* [A-Z][a-z]+ */
    PT_NUM,        /* [0-9]+ */
    PT_VER,        /* v[0-9]+\.[0-9]+ */
    PT_PAIR,       /* (w1|w2) */
    PT_HYPHEN,     /* [a-z]+-[a-z]+ */
    PT_LOG,        /* (error|warning|note): [a-z]+ */
    PT_REP,        /* [a-z]{4} */
    PT_MAIL,       /* [a-z]+@[a-z]+\.[a-z]+ */
    PT_QUOTED,     /* "[^"]+" */
    PT_EMPH,       /* \*[^*]+\* */
    PT_URL,        /* [a-z]+://[a-z]+ */
    PT_HEAD,       /* ^## [A-Z] */
    PT_CODE,       /* `[^`]+` */
    PT_NTEMPLATES
};

static void pat_word(cbuf *b, rng_t *r, char (*words)[16], unsigned nw)
{
    buf_puts(b, words[rng_below(r, nw)]);
}

static void pat_pair(cbuf *b, rng_t *r, char (*words)[16], unsigned nw)
{
    unsigned a = rng_below(r, nw), c = rng_below(r, nw);

    buf_putc(b, '(');
    buf_puts(b, words[a]);
    buf_putc(b, '|');
    buf_puts(b, words[c == a ? (a + 1) % nw : c]);
    buf_putc(b, ')');
}

static void pattern_of(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                       unsigned tmpl)
{
    switch (tmpl) {
    case PT_CAP:
        buf_puts(b, "[A-Z][a-z]+");
        break;
    case PT_NUM:
        buf_puts(b, "[0-9]+");
        break;
    case PT_VER:
        buf_puts(b, "v[0-9]+\\.[0-9]+");
        break;
    case PT_PAIR:
        pat_pair(b, r, words, nw);
        break;
    case PT_HYPHEN:
        buf_puts(b, "[a-z]+-[a-z]+");
        break;
    case PT_LOG:
        buf_puts(b, "(error|warning|note): [a-z]+");
        break;
    case PT_REP:
        buf_puts(b, "[a-z]{4}");
        break;
    case PT_MAIL:
        buf_puts(b, "[a-z]+@[a-z]+\\.[a-z]+");
        break;
    case PT_QUOTED:
        buf_puts(b, "\"[^\"]+\"");
        break;
    case PT_EMPH:
        buf_puts(b, "\\*[^*]+\\*");
        break;
    case PT_URL:
        buf_puts(b, "[a-z]+://[a-z]+");
        break;
    case PT_HEAD:
        buf_puts(b, "^## [A-Z]");
        break;
    case PT_CODE:
        buf_puts(b, "`[^`]+`");
        break;
    default:
        pat_word(b, r, words, nw);
        break;
    }
}

static void pick_patterns(cbuf pats[TP_PATTERNS], rng_t *r,
                          char (*words)[16], unsigned nw, unsigned arch)
{
    unsigned sig[3], nsig;
    unsigned used[PT_NTEMPLATES];
    unsigned nused = 0;

    switch (arch) {
    case 0: /* API documentation */
        sig[0] = PT_CODE;
        sig[1] = PT_HEAD;
        nsig = 2;
        break;
    case 1: /* blog post */
        sig[0] = PT_EMPH;
        sig[1] = PT_URL;
        sig[2] = PT_MAIL;
        nsig = 3;
        break;
    case 2: /* changelog */
        sig[0] = PT_VER;
        sig[1] = PT_NUM;
        nsig = 2;
        break;
    case 3: /* log digest */
        sig[0] = PT_LOG;
        sig[1] = PT_REP;
        nsig = 2;
        break;
    case 4: /* FAQ */
        sig[0] = PT_QUOTED;
        sig[1] = PT_NUM;
        nsig = 2;
        break;
    default: /* README */
        sig[0] = PT_URL;
        sig[1] = PT_HYPHEN;
        sig[2] = PT_HEAD;
        nsig = 3;
        break;
    }
    for (unsigned k = 0; k < nsig; k++) {
        pattern_of(&pats[k], r, words, nw, sig[k]);
        used[nused++] = sig[k];
    }
    for (unsigned k = nsig; k < TP_PATTERNS; k++) {
        unsigned t;

        do {
            t = rng_below(r, PT_NTEMPLATES);
        } while (t == PT_HEAD); /* anchored generic patterns read poorly */
        for (unsigned u = 0; u < nused; u++)
            if (used[u] == t)
                t = PT_WORD; /* template already used: literal word */
        used[nused++] = t;
        pattern_of(&pats[k], r, words, nw, t);
    }
}

/* Inline "special" tokens — the regex targets the archetypes exist to
 * exercise. Each one is a well-formed Markdown fragment per the
 * documented inline rules. */
static void gen_special(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                        unsigned kind)
{
    char w[16], w2[16];

    switch (kind) {
    case 0: /* version */
        buf_putc(b, 'v');
        buf_putu(b, 1 + rng_below(r, 12));
        buf_putc(b, '.');
        buf_putu(b, rng_below(r, 100));
        break;
    case 1: /* plain number */
        buf_putu(b, rng_below(r, 100000u));
        break;
    case 2: /* quoted phrase */
        buf_putc(b, '"');
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, ' ');
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, '"');
        break;
    case 3: /* email */
        make_word(w, r, 0);
        buf_puts(b, w);
        buf_putc(b, '@');
        make_word(w2, r, 0);
        buf_puts(b, w2);
        buf_puts(b, ".tld");
        break;
    case 4: /* URL */
        buf_puts(b, "http://");
        make_word(w, r, 0);
        buf_puts(b, w);
        buf_puts(b, ".tld");
        break;
    case 5: /* hyphenated pair */
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, '-');
        buf_puts(b, words[rng_below(r, nw)]);
        break;
    case 6: /* emphasized word */
        buf_putc(b, '*');
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, '*');
        break;
    case 7: /* strong phrase */
        buf_puts(b, "**");
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, ' ');
        buf_puts(b, words[rng_below(r, nw)]);
        buf_puts(b, "**");
        break;
    case 8: /* code span */
        buf_putc(b, '`');
        buf_puts(b, words[rng_below(r, nw)]);
        buf_putc(b, '`');
        break;
    default: /* log token */
        buf_puts(b, "error: ");
        buf_puts(b, words[rng_below(r, nw)]);
        break;
    }
}

static void gen_sentence(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                         unsigned arch)
{
    unsigned n = TP_WORDS_LO + rng_below(r, TP_WORDS_RN);
    char w[16];

    for (unsigned i = 0; i < n; i++) {
        if (i)
            buf_putc(b, ' ');
        if (i > 0 && rng_below(r, 100) < 22) {
            /* an inline special, archetype-flavoured */
            unsigned k;

            switch (arch) {
            case 0:
                k = (unsigned)(rng_below(r, 2) ? 8 : 6);
                break;
            case 1:
                k = (unsigned)(3 + rng_below(r, 5)); /* mail..strong */
                break;
            case 2:
                k = (unsigned)(rng_below(r, 2) ? 0 : 1);
                break;
            case 3:
                k = (unsigned)(rng_below(r, 3) ? 9 : 1);
                break;
            case 4:
                k = (unsigned)(rng_below(r, 2) ? 2 : 1);
                break;
            default:
                k = (unsigned)(rng_below(r, 7));
                if (k == 9)
                    k = 4;
                break;
            }
            gen_special(b, r, words, nw, k);
        } else if (i == 0) {
            make_capword(w, r);
            buf_puts(b, w);
        } else {
            buf_puts(b, words[rng_below(r, nw)]);
        }
    }
    buf_putc(b, rng_below(r, 100) < 85 ? '.' :
                    (rng_below(r, 2) ? '?' : '!'));
}

static void gen_paragraph(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                          unsigned arch)
{
    unsigned n = TP_SENTS_LO + rng_below(r, TP_SENTS_RN);

    for (unsigned i = 0; i < n; i++) {
        if (i)
            buf_putc(b, ' ');
        gen_sentence(b, r, words, nw, arch);
    }
    buf_putc(b, '\n');
}

static void gen_fenced(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                       unsigned arch)
{
    unsigned n = TP_FENCE_LO + rng_below(r, TP_FENCE_RN);
    char w[16];

    buf_puts(b, "```\n");
    for (unsigned i = 0; i < n; i++) {
        if (arch == 3) { /* log digest: error/warning/note lines */
            static const char *const LEVELS[] = { "error", "warning",
                                                  "note" };

            buf_puts(b, LEVELS[rng_below(r, 3)]);
            buf_puts(b, ": ");
            for (unsigned j = 0, m = 1 + rng_below(r, 4); j < m; j++) {
                if (j)
                    buf_putc(b, ' ');
                buf_puts(b, words[rng_below(r, nw)]);
            }
            buf_putc(b, '\n');
        } else { /* config-ish lines: name = value */
            make_word(w, r, 1);
            buf_puts(b, w);
            buf_puts(b, " = ");
            buf_putu(b, rng_below(r, 100000u));
            buf_putc(b, '\n');
        }
    }
    buf_puts(b, "```\n");
}

static void gen_list(cbuf *b, rng_t *r, char (*words)[16], unsigned nw,
                     unsigned arch)
{
    unsigned n = TP_LIST_LO + rng_below(r, TP_LIST_RN);
    char w[16];
    int ordered = (arch == 2 || arch == 4);

    for (unsigned i = 0; i < n; i++) {
        if (ordered)
            buf_putu(b, i + 1);
        else
            buf_putc(b, '-');
        buf_puts(b, ordered ? ". " : " ");
        if (rng_below(r, 100) < 30) {
            gen_special(b, r, words, nw,
                        (unsigned)(rng_below(r, 10)));
            buf_putc(b, ' ');
        }
        if (arch == 4 && rng_below(r, 2)) { /* FAQ questions */
            make_capword(w, r);
            buf_puts(b, w);
            buf_putc(b, ' ');
            buf_puts(b, words[rng_below(r, nw)]);
            buf_puts(b, "? ");
            buf_puts(b, words[rng_below(r, nw)]);
        } else {
            for (unsigned j = 0, m = 2 + rng_below(r, 6); j < m; j++) {
                if (j)
                    buf_putc(b, ' ');
                buf_puts(b, words[rng_below(r, nw)]);
            }
        }
        buf_putc(b, '\n');
    }
}

static void gen_quote(cbuf *b, rng_t *r, char (*words)[16], unsigned nw)
{
    unsigned n = 1 + rng_below(r, 3);

    for (unsigned i = 0; i < n; i++) {
        buf_puts(b, "> ");
        gen_sentence(b, r, words, nw, 1);
        if (i + 1 < n)
            buf_putc(b, '\n');
    }
    buf_putc(b, '\n');
}

/* The per-document vocabulary: syllabic, purely alphabetic words, no
 * duplicates — the raw material of sentences and patterns alike. */
static void build_words(char (*words)[16], rng_t *r)
{
    for (unsigned i = 0; i < TP_WORDS; i++) {
        int dup;

        do {
            make_word(words[i], r, i % 4 == 3);
            dup = 0;
            for (unsigned j = 0; j < i && !dup; j++)
                if (strcmp(words[j], words[i]) == 0)
                    dup = 1;
        } while (dup);
    }
}

static void gen_doc(cbuf *b, rng_t *r, unsigned doc,
                    char (*words)[16])
{
    unsigned arch = doc % 6;
    char w[16], w2[16];
    unsigned sections = TP_SECTIONS_LO + rng_below(r, TP_SECTIONS_RN);

    /* title */
    buf_puts(b, "# ");
    make_capword(w, r);
    buf_puts(b, w);
    buf_putc(b, ' ');
    buf_puts(b, words[rng_below(r, TP_WORDS)]);
    buf_puts(b, "\n\n");
    for (unsigned s = 0; s < sections; s++) {
        unsigned m;

        buf_puts(b, "## ");
        make_capword(w, r);
        buf_puts(b, w);
        if (arch == 2) { /* changelog section heads carry versions */
            buf_puts(b, " v");
            buf_putu(b, 1 + rng_below(r, 12));
            buf_putc(b, '.');
            buf_putu(b, rng_below(r, 100));
        } else if (arch == 4 && rng_below(r, 2)) {
            buf_putc(b, '?');
        }
        buf_putc(b, '\n');
        if (arch == 5 && rng_below(r, 3) == 0)
            buf_puts(b, "---\n\n");
        if (rng_below(r, 100) < (arch == 3 ? 40u : 25u))
            gen_fenced(b, r, words, TP_WORDS, arch);
        m = TP_PARAS_LO + rng_below(r, TP_PARAS_RN);
        for (unsigned i = 0; i < m; i++) {
            if (rng_below(r, 100) < 20) {
                if (arch == 1 || arch == 5)
                    gen_quote(b, r, words, TP_WORDS);
                else
                    gen_list(b, r, words, TP_WORDS, arch);
            } else {
                gen_paragraph(b, r, words, TP_WORDS, arch);
            }
            if (rng_below(r, 100) < 15)
                buf_putc(b, '\n');
        }
        if (rng_below(r, 100) < 30) {
            make_word(w, r, 0);
            make_word(w2, r, 0);
            buf_puts(b, "See [");
            buf_puts(b, w);
            buf_puts(b, "](http://");
            buf_puts(b, w2);
            buf_puts(b, ".tld/) for more.\n\n");
        }
    }
}

/* --- context, reference pass, run ----------------------------------------------*/

struct tp_ctx {
    unsigned seed;
    struct {
        char *src;
        size_t len;
        re_prog pats[TP_PATTERNS];
        char pat_src[TP_PATTERNS][TP_PAT_MAX + 1];
        uint32_t counts[TP_PATTERNS];
        uint8_t *html;
        size_t html_len;
        uint8_t digest[20];
    } doc[TP_DOCS];
    uint64_t task_bytes;
    re_machine tool_machine; /* test-hook scratch */
    re_prog tool_prog;
    cbuf tool_out;
};

tp_ctx *tp_prepare(unsigned seed)
{
    tp_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;
    cbuf out = { NULL, 0, 0, 0 };

    if (!ctx)
        return NULL;
    ctx->seed = seed;
    rng_init(&rng, (uint64_t)seed * 0xBF58476D1CE4E5B9ull +
                       0x94D049BB133111EBull);
    for (unsigned d = 0; d < TP_DOCS; d++) {
        cbuf b = { NULL, 0, 0, 0 };
        cbuf pats[TP_PATTERNS];
        char pat_buf[TP_PATTERNS][TP_PAT_MAX + 4];
        char words[TP_WORDS][16];
        unsigned arch = d % 6;
        rng_t pr;

        /* every document owns two streams: one for the prose, one for
         * the pattern set — the inputs stay independent of each other */
        rng_init(&pr, (uint64_t)seed * 0x9E3779B97F4A7C15ull +
                          0x165667B19E3779F9ull + (uint64_t)d * 2);
        build_words(words, &pr);
        gen_doc(&b, &rng, d, words);
        if (b.oom || !b.p) {
            free(b.p);
            tp_free(ctx);
            return NULL;
        }
        ctx->doc[d].src = (char *)b.p;
        ctx->doc[d].len = b.n;

        rng_init(&pr, (uint64_t)seed * 0x9E3779B97F4A7C15ull +
                          0x165667B19E3779F9ull + (uint64_t)d * 2 + 1);
        for (unsigned k = 0; k < TP_PATTERNS; k++) {
            /* pre-set capacity: the writer fills the stack buffer in
             * place and must never realloc it */
            pats[k].p = (uint8_t *)pat_buf[k];
            pats[k].n = 0;
            pats[k].cap = sizeof(pat_buf[k]);
            pats[k].oom = 0;
        }
        pick_patterns(pats, &pr, words, TP_WORDS, arch);
        for (unsigned k = 0; k < TP_PATTERNS; k++) {
            size_t pl = pats[k].n;

            if (pl == 0 || pl > TP_PAT_MAX) {
                tp_free(ctx);
                return NULL;
            }
            memcpy(ctx->doc[d].pat_src[k], pat_buf[k], pl);
            ctx->doc[d].pat_src[k][pl] = 0;
            if (re_compile(&ctx->doc[d].pats[k], pat_buf[k], pl)) {
                tp_free(ctx);
                return NULL;
            }
        }
        /* reference conversion + digest */
        buf_reset(&out);
        md_convert(&out, (const uint8_t *)b.p, b.n);
        if (out.oom) {
            tp_free(ctx);
            return NULL;
        }
        ctx->doc[d].html_len = out.n;
        ctx->doc[d].html = malloc(out.n ? out.n : 1);
        if (!ctx->doc[d].html) {
            tp_free(ctx);
            return NULL;
        }
        memcpy(ctx->doc[d].html, out.p, out.n);
        sha1(ctx->doc[d].html, out.n, ctx->doc[d].digest);
        /* reference counts + the exact task size: every document is
         * scanned once per pattern (regex stage) and once whole
         * (Markdown stage) */
        for (unsigned k = 0; k < TP_PATTERNS; k++) {
            long c = re_count(&ctx->doc[d].pats[k], &ctx->tool_machine,
                              (const uint8_t *)b.p, b.n);

            if (c < 0) {
                tp_free(ctx);
                return NULL;
            }
            ctx->doc[d].counts[k] = (uint32_t)c;
            ctx->task_bytes += b.n;
        }
        ctx->task_bytes += b.n;
    }
    free(out.p);
    return ctx;
}

void tp_free(tp_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned d = 0; d < TP_DOCS; d++) {
        free(ctx->doc[d].src);
        free(ctx->doc[d].html);
    }
    machine_free(&ctx->tool_machine);
    free(ctx->tool_out.p);
    free(ctx);
}

/* One thread's slice: contiguous documents, own arenas — threads never
 * share writable state (shared-task harness contract). */
typedef struct {
    tp_ctx *ctx;
    re_machine machine;
    cbuf out;
    uint32_t begin, end;
} tp_slice;

static int tp_cycle(void *arg)
{
    tp_slice *sl = arg;

    for (uint32_t d = sl->begin; d < sl->end; d++) {
        const uint8_t *src = (const uint8_t *)sl->ctx->doc[d].src;
        size_t len = sl->ctx->doc[d].len;

        for (unsigned k = 0; k < TP_PATTERNS; k++) {
            long c = re_count(&sl->ctx->doc[d].pats[k], &sl->machine,
                              src, len);

            if (c < 0 || (uint32_t)c != sl->ctx->doc[d].counts[k])
                return -1;
        }
        buf_reset(&sl->out);
        md_convert(&sl->out, src, len);
        if (sl->out.oom)
            return -1;
        uint8_t dig[20];

        sha1(sl->out.p, sl->out.n, dig);
        if (sl->out.n != sl->ctx->doc[d].html_len ||
            memcmp(dig, sl->ctx->doc[d].digest, 20) != 0)
            return -1;
    }
    return 0;
}

int tp_run(tp_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps, uint64_t *bytes)
{
    unsigned t_count = *threads;
    tp_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (t_count > TP_DOCS) /* one document per thread at least */
        t_count = TP_DOCS;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        uint32_t per = TP_DOCS / t_count;
        unsigned extra = TP_DOCS % t_count;
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

    rc = harness_run(t_count, runs, tp_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)ctx->task_bytes / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mbps = lb_median(rates, runs);
        *threads = t_count;
        *bytes = ctx->task_bytes;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            machine_free(&slices[t].machine);
            free(slices[t].out.p);
        }
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors and test hooks --------------------------------------------------*/

uint32_t tp_docs(const tp_ctx *ctx)
{
    return ctx ? TP_DOCS : 0;
}

uint64_t tp_bytes(const tp_ctx *ctx)
{
    return ctx ? ctx->task_bytes : 0;
}

uint64_t tp_doc_len(const tp_ctx *ctx, unsigned doc)
{
    if (!ctx || doc >= TP_DOCS)
        return 0;
    return ctx->doc[doc].len;
}

const char *tp_doc_source(const tp_ctx *ctx, unsigned doc)
{
    if (!ctx || doc >= TP_DOCS)
        return NULL;
    return ctx->doc[doc].src;
}

const char *tp_doc_pattern(const tp_ctx *ctx, unsigned doc, unsigned pat)
{
    if (!ctx || doc >= TP_DOCS || pat >= TP_PATTERNS)
        return NULL;
    return ctx->doc[doc].pat_src[pat];
}

uint32_t tp_doc_matches(const tp_ctx *ctx, unsigned doc, unsigned pat)
{
    if (!ctx || doc >= TP_DOCS || pat >= TP_PATTERNS)
        return 0;
    return ctx->doc[doc].counts[pat];
}

size_t tp_doc_html_len(const tp_ctx *ctx, unsigned doc)
{
    if (!ctx || doc >= TP_DOCS)
        return 0;
    return ctx->doc[doc].html_len;
}

const char *tp_doc_html(const tp_ctx *ctx, unsigned doc)
{
    if (!ctx || doc >= TP_DOCS)
        return NULL;
    return (const char *)ctx->doc[doc].html;
}

uint64_t tp_doc_digest(const tp_ctx *ctx, unsigned doc)
{
    if (!ctx || doc >= TP_DOCS)
        return 0;
    uint64_t v = 0;

    for (int i = 0; i < 8; i++)
        v |= (uint64_t)ctx->doc[doc].digest[i] << (8 * i);
    return v;
}

long tp_regex_count(tp_ctx *ctx, const char *pattern,
                    const char *text, size_t len)
{
    if (!ctx || !pattern || !text)
        return -1;
    if (re_compile(&ctx->tool_prog, pattern, strlen(pattern)))
        return -1;
    return re_count(&ctx->tool_prog, &ctx->tool_machine,
                    (const uint8_t *)text, len);
}

long tp_markdown(tp_ctx *ctx, const char *markdown, size_t len,
                 uint8_t *out, size_t cap)
{
    if (!ctx || !markdown)
        return -1;
    buf_reset(&ctx->tool_out);
    md_convert(&ctx->tool_out, (const uint8_t *)markdown, len);
    if (ctx->tool_out.oom)
        return -1;
    if (!out || ctx->tool_out.n > cap)
        return -1;
    memcpy(out, ctx->tool_out.p, ctx->tool_out.n);
    return (long)ctx->tool_out.n;
}
