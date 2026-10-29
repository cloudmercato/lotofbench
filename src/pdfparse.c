/* LotofBench — PDF reader (workload #4, internal).
 *
 * A strict, from-scratch parser for the PDF 1.4 subset the corpus
 * generator emits: classic xref table, indirect objects, arrays,
 * dictionaries, names, numbers, strings (skipped, never interpreted),
 * and streams with a direct /Length. Anything outside the subset —
 * xref streams, indirect /Length, broken offsets, dangling references —
 * is a hard parse error. Strictness is the point: the render workload
 * must never score on input it did not fully understand. */

#include "pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- allocation arena (type lives in pdf.h) ----------------------------------*/

static void *arena_alloc(pdf_file *f, size_t sz)
{
    if (f->arena.n == f->arena.cap) {
        size_t cap = f->arena.cap ? f->arena.cap * 2 : 64;
        void **slots = realloc(f->arena.slots, cap * sizeof(*slots));
        if (!slots)
            return NULL;
        f->arena.slots = slots;
        f->arena.cap = cap;
    }
    void *p = calloc(1, sz);
    if (!p)
        return NULL;
    f->arena.slots[f->arena.n++] = p;
    return p;
}

static void arena_release(pdf_arena *a)
{
    for (size_t i = 0; i < a->n; i++)
        free(a->slots[i]);
    free(a->slots);
    memset(a, 0, sizeof(*a));
}

/* --- lexer primitives ---------------------------------------------------------*/

typedef struct {
    const uint8_t *buf;
    size_t         len, pos;
    pdf_file      *f;
} pdf_lex;

static void lex_fail(pdf_lex *lx, const char *msg)
{
    if (!lx->f->error[0])
        snprintf(lx->f->error, sizeof(lx->f->error), "%s (at offset %zu)",
                 msg, lx->pos);
}

static int is_ws(int c)
{
    return c == '\0' || c == '\t' || c == '\n' || c == '\f' || c == '\r' ||
           c == ' ';
}

static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static void skip_ws(pdf_lex *lx)
{
    while (lx->pos < lx->len) {
        int c = lx->buf[lx->pos];
        if (is_ws(c)) {
            lx->pos++;
        } else if (c == '%') { /* comment runs to end of line */
            while (lx->pos < lx->len && lx->buf[lx->pos] != '\n' &&
                   lx->buf[lx->pos] != '\r')
                lx->pos++;
        } else {
            break;
        }
    }
}

/* Read a bare keyword ([A-Za-z]+) without consuming it. Returns its
 * length, or 0 when the next token is not a keyword. */
static size_t peek_word(pdf_lex *lx, char word[16])
{
    size_t save = lx->pos, n = 0;

    skip_ws(lx);
    while (lx->pos < lx->len && n < 15) {
        int c = lx->buf[lx->pos];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            word[n++] = (char)c;
        else
            break;
        lx->pos++;
    }
    lx->pos = save;
    word[n] = '\0';
    return n;
}

/* Like peek_word, but leaves the cursor just after the word. */
static size_t skip_word(pdf_lex *lx, char word[16])
{
    size_t n = peek_word(lx, word);

    skip_ws(lx);
    lx->pos += n;
    return n;
}

static int accept_word(pdf_lex *lx, const char *kw)
{
    char word[16];

    skip_word(lx, word);
    return strcmp(word, kw) == 0;
}

/* --- value parsing --------------------------------------------------------------*/

static pdf_obj *parse_value(pdf_lex *lx);

static pdf_obj *obj_new(pdf_lex *lx, pdf_obj_kind kind)
{
    pdf_obj *o = arena_alloc(lx->f, sizeof(*o));

    if (o)
        o->kind = kind;
    return o;
}

static int lex_int(pdf_lex *lx, int64_t *out)
{
    size_t save = lx->pos;
    int neg = 0;
    int64_t v = 0;
    int digits = 0;

    skip_ws(lx);
    if (lx->pos < lx->len && (lx->buf[lx->pos] == '+' || lx->buf[lx->pos] == '-'))
        neg = lx->buf[lx->pos++] == '-';
    while (lx->pos < lx->len && lx->buf[lx->pos] >= '0' &&
           lx->buf[lx->pos] <= '9') {
        if (v < (INT64_MAX - 9) / 10)
            v = v * 10 + (lx->buf[lx->pos] - '0');
        else
            v = INT64_MAX; /* saturate; range checks happen upstream */
        digits++;
        lx->pos++;
    }
    if (!digits) {
        lx->pos = save;
        return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

/* Parse `num gen R` after a leading integer was consumed. Restores the
 * cursor when the tokens do not form a reference. */
static pdf_obj *try_ref(pdf_lex *lx, int64_t first)
{
    size_t save = lx->pos;
    int64_t num = first, gen = 0;
    char word[16];

    if (!lex_int(lx, &gen))
        goto plain;
    skip_word(lx, word); /* consumes ws + 'R', leaves cursor past it */
    if (strcmp(word, "R") != 0)
        goto plain;
    if (num < 0 || num > UINT32_MAX - 1 || gen < 0 || gen > 65535)
        goto plain;
    pdf_obj *o = obj_new(lx, PDF_REF);
    if (!o)
        return NULL;
    o->u.ref.num = (int32_t)num;
    o->u.ref.gen = (int32_t)gen;
    return o;

plain:
    lx->pos = save;
    pdf_obj *oi = obj_new(lx, PDF_INT);
    if (!oi)
        return NULL;
    oi->u.i = first;
    return oi;
}

static pdf_obj *parse_number(pdf_lex *lx)
{
    size_t save = lx->pos;
    int neg = 0;
    uint64_t ip = 0;
    int digits = 0;

    skip_ws(lx);
    if (lx->pos < lx->len &&
        (lx->buf[lx->pos] == '+' || lx->buf[lx->pos] == '-'))
        neg = lx->buf[lx->pos++] == '-';
    while (lx->pos < lx->len && lx->buf[lx->pos] >= '0' &&
           lx->buf[lx->pos] <= '9') {
        ip = ip * 10 + (uint64_t)(lx->buf[lx->pos] - '0');
        digits++;
        lx->pos++;
    }
    if (lx->pos < lx->len &&
        (lx->buf[lx->pos] == '.' || lx->buf[lx->pos] == 'e' ||
         lx->buf[lx->pos] == 'E')) {
        /* real number: mantissa fraction and optional exponent, parsed
         * with plain arithmetic (no locale, no strtod edge cases) */
        double v = (double)ip;
        if (lx->buf[lx->pos] == '.') {
            lx->pos++;
            double scale = 0.1;
            while (lx->pos < lx->len && lx->buf[lx->pos] >= '0' &&
                   lx->buf[lx->pos] <= '9') {
                v += (lx->buf[lx->pos] - '0') * scale;
                scale *= 0.1;
                lx->pos++;
            }
        }
        if (lx->pos < lx->len &&
            (lx->buf[lx->pos] == 'e' || lx->buf[lx->pos] == 'E')) {
            lx->pos++;
            int eneg = 0;
            if (lx->pos < lx->len &&
                (lx->buf[lx->pos] == '+' || lx->buf[lx->pos] == '-'))
                eneg = lx->buf[lx->pos++] == '-';
            int exp = 0;
            while (lx->pos < lx->len && lx->buf[lx->pos] >= '0' &&
                   lx->buf[lx->pos] <= '9') {
                if (exp < 100)
                    exp = exp * 10 + (lx->buf[lx->pos] - '0');
                lx->pos++;
            }
            if (eneg)
                exp = -exp;
            while (exp > 0) {
                v *= 10.0;
                exp--;
            }
            while (exp < 0) {
                v *= 0.1;
                exp++;
            }
        }
        pdf_obj *o = obj_new(lx, PDF_REAL);
        if (!o)
            return NULL;
        o->u.d = neg ? -v : v;
        return o;
    }
    if (!digits) {
        lx->pos = save;
        return NULL;
    }
    return try_ref(lx, neg ? -(int64_t)ip : (int64_t)ip);
}

/* Decode a name into the arena; returns NULL on allocation failure. */
static pdf_obj *parse_name(pdf_lex *lx)
{
    uint8_t raw[256];
    size_t n = 0;

    lx->pos++; /* '/' */
    while (lx->pos < lx->len && !is_ws(lx->buf[lx->pos]) &&
           !is_delim(lx->buf[lx->pos])) {
        int c = lx->buf[lx->pos];
        if (c == '#') {
            if (lx->pos + 2 >= lx->len ||
                !((lx->buf[lx->pos + 1] >= '0' && lx->buf[lx->pos + 1] <= '9') ||
                  (lx->buf[lx->pos + 1] >= 'A' && lx->buf[lx->pos + 1] <= 'F') ||
                  (lx->buf[lx->pos + 1] >= 'a' && lx->buf[lx->pos + 1] <= 'f')))
                return NULL;
            unsigned hi = (unsigned)lx->buf[lx->pos + 1], lo = (unsigned)lx->buf[lx->pos + 2];
            hi = hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10;
            c = (int)((hi << 4) | lo);
            lx->pos += 3;
        } else {
            lx->pos++;
        }
        if (n >= sizeof(raw))
            return NULL;
        raw[n++] = (uint8_t)c;
    }
    if (n == 0)
        return NULL;
    pdf_obj *o = obj_new(lx, PDF_NAME);
    if (!o)
        return NULL;
    uint8_t *copy = arena_alloc(lx->f, n);
    if (!copy)
        return NULL;
    memcpy(copy, raw, n);
    o->u.str.ptr = copy;
    o->u.str.len = (uint32_t)n;
    return o;
}

/* Skip a literal (...) or hex <...> string; the contents are never
 * interpreted by this parser. */
static pdf_obj *skip_string(pdf_lex *lx, int hex)
{
    pdf_obj *o = obj_new(lx, PDF_STRING);

    if (!o)
        return NULL;
    o->u.str.ptr = lx->buf + lx->pos;
    o->u.str.len = 0;
    if (hex) {
        lx->pos++; /* '<' */
        while (lx->pos < lx->len && lx->buf[lx->pos] != '>')
            lx->pos++;
        if (lx->pos >= lx->len) {
            lex_fail(lx, "unterminated hex string");
            return NULL;
        }
        lx->pos++;
    } else {
        lx->pos++; /* '(' */
        int depth = 1;
        while (lx->pos < lx->len && depth > 0) {
            int c = lx->buf[lx->pos];
            if (c == '\\') {
                lx->pos += 2;
                continue;
            }
            if (c == '(')
                depth++;
            if (c == ')')
                depth--;
            lx->pos++;
        }
        if (depth != 0) {
            lex_fail(lx, "unterminated string");
            return NULL;
        }
    }
    return o;
}

static pdf_obj *parse_dict(pdf_lex *lx);

static pdf_obj *parse_array(pdf_lex *lx)
{
    lx->pos++; /* '[' */
    pdf_obj *o = obj_new(lx, PDF_ARRAY);
    if (!o)
        return NULL;
    uint32_t cap = 8;
    pdf_obj **items = arena_alloc(lx->f, cap * sizeof(*items));
    if (!items)
        return NULL;
    o->u.arr.items = items;
    for (;;) {
        skip_ws(lx);
        if (lx->pos >= lx->len) {
            lex_fail(lx, "unterminated array");
            return NULL;
        }
        if (lx->buf[lx->pos] == ']') {
            lx->pos++;
            break;
        }
        pdf_obj *item = parse_value(lx);
        if (!item)
            return NULL;
        if (o->u.arr.n == cap) {
            if (cap > 4u * 1024u * 1024u) {
                lex_fail(lx, "array too large");
                return NULL;
            }
            uint32_t ncap = cap * 2;
            pdf_obj **nitems = arena_alloc(lx->f, ncap * sizeof(*nitems));
            if (!nitems)
                return NULL;
            memcpy(nitems, items, o->u.arr.n * sizeof(*items));
            items = nitems;
            o->u.arr.items = nitems;
            cap = ncap;
        }
        items[o->u.arr.n++] = item;
    }
    return o;
}

static pdf_obj *parse_value(pdf_lex *lx)
{
    skip_ws(lx);
    if (lx->pos >= lx->len) {
        lex_fail(lx, "unexpected end of file");
        return NULL;
    }
    int c = lx->buf[lx->pos];
    if (c == '/' || c == '[' || c == '(' || c == '<' || c == '+' || c == '-' ||
        c == '.' || (c >= '0' && c <= '9'))
        return c == '/'    ? parse_name(lx)
               : c == '['  ? parse_array(lx)
               : c == '('  ? skip_string(lx, 0)
               : c == '<'  ? (lx->pos + 1 < lx->len &&
                                      lx->buf[lx->pos + 1] == '<'
                                  ? parse_dict(lx)
                                  : skip_string(lx, 1))
                           : parse_number(lx);
    char word[16];

    skip_word(lx, word); /* consumes the keyword, cursor left past it */
    if (!strcmp(word, "true") || !strcmp(word, "false")) {
        pdf_obj *o = obj_new(lx, PDF_BOOL);
        if (!o)
            return NULL;
        o->u.b = word[0] == 't';
        return o;
    }
    if (!strcmp(word, "null"))
        return obj_new(lx, PDF_NULL);
    lex_fail(lx, "unexpected token");
    return NULL;
}

static pdf_obj *parse_dict(pdf_lex *lx)
{
    lx->pos += 2; /* '<<' */
    pdf_obj *o = obj_new(lx, PDF_DICT);
    if (!o)
        return NULL;
    uint32_t cap = 8;
    pdf_obj **keys = arena_alloc(lx->f, cap * sizeof(*keys));
    pdf_obj **vals = arena_alloc(lx->f, cap * sizeof(*vals));
    if (!keys || !vals)
        return NULL;
    o->u.dict.keys = keys;
    o->u.dict.vals = vals;
    for (;;) {
        skip_ws(lx);
        if (lx->pos + 1 < lx->len && lx->buf[lx->pos] == '<' &&
            lx->buf[lx->pos + 1] == '<') {
            lex_fail(lx, "nested dict without key");
            return NULL;
        }
        if (lx->pos + 1 < lx->len && lx->buf[lx->pos] == '>' &&
            lx->buf[lx->pos + 1] == '>') {
            lx->pos += 2;
            break;
        }
        if (lx->pos >= lx->len || lx->buf[lx->pos] != '/') {
            lex_fail(lx, "dictionary key expected");
            return NULL;
        }
        pdf_obj *key = parse_name(lx);
        pdf_obj *val = key ? parse_value(lx) : NULL;
        if (!key || !val)
            return NULL;
        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < o->u.dict.n; i++)
            if (keys[i]->u.str.len == key->u.str.len &&
                !memcmp(keys[i]->u.str.ptr, key->u.str.ptr, key->u.str.len))
                found = i;
        if (found != UINT32_MAX) { /* last duplicate wins */
            vals[found] = val;
            continue;
        }
        if (o->u.dict.n == cap) {
            if (cap > 4u * 1024u * 1024u) {
                lex_fail(lx, "dictionary too large");
                return NULL;
            }
            uint32_t ncap = cap * 2;
            pdf_obj **nkeys = arena_alloc(lx->f, ncap * sizeof(*nkeys));
            pdf_obj **nvals = arena_alloc(lx->f, ncap * sizeof(*nvals));
            if (!nkeys || !nvals)
                return NULL;
            memcpy(nkeys, keys, o->u.dict.n * sizeof(*keys));
            memcpy(nvals, vals, o->u.dict.n * sizeof(*vals));
            keys = nkeys;
            vals = nvals;
            o->u.dict.keys = nkeys;
            o->u.dict.vals = nvals;
            cap = ncap;
        }
        keys[o->u.dict.n] = key;
        vals[o->u.dict.n] = val;
        o->u.dict.n++;
    }
    return o;
}

/* --- object loading -------------------------------------------------------------*/

static int parse_stream(pdf_lex *lx, pdf_obj *dict, uint32_t idx)
{
    int64_t length = pdf_dict_get_int(dict, "Length", -1);

    if (length < 0) {
        lex_fail(lx, "stream without a direct integer /Length");
        return -1;
    }
    if ((uint64_t)length > lx->len - lx->pos) {
        lex_fail(lx, "stream /Length overruns the file");
        return -1;
    }
    /* EOL after the `stream` keyword: \n, \r\n or \r */
    if (lx->pos < lx->len && lx->buf[lx->pos] == '\r')
        lx->pos++;
    if (lx->pos < lx->len && lx->buf[lx->pos] == '\n')
        lx->pos++;
    pdf_stream *st = arena_alloc(lx->f, sizeof(*st));
    if (!st)
        return -1;
    st->dict = dict;
    st->data = lx->buf + lx->pos;
    st->len = (uint32_t)length;
    lx->pos += (size_t)length;
    skip_ws(lx);
    if (!accept_word(lx, "endstream")) {
        lex_fail(lx, "missing endstream");
        return -1;
    }
    lx->f->objs[idx].stream = st;
    return 0;
}

static int load_object(pdf_lex *lx, uint32_t num, uint32_t gen, size_t ofs)
{
    lx->pos = ofs;
    int64_t n = 0, g = 0;

    if (!lex_int(lx, &n) || n != (int64_t)num)
        goto bad;
    if (!lex_int(lx, &g) || g != (int64_t)gen)
        goto bad;
    if (!accept_word(lx, "obj"))
        goto bad;
    pdf_obj *o = parse_value(lx);
    if (!o)
        return -1;
    uint32_t idx = num;
    lx->f->objs[idx].number = num;
    lx->f->objs[idx].obj = o;
    char word[16];
    size_t save = lx->pos;

    skip_word(lx, word);
    if (!strcmp(word, "stream")) {
        if (parse_stream(lx, o, idx) != 0)
            return -1;
    } else {
        lx->pos = save; /* not a stream keyword: put the word back */
    }
    if (!accept_word(lx, "endobj")) {
        lex_fail(lx, "missing endobj");
        return -1;
    }
    return 0;

bad:
    lex_fail(lx, "object header mismatch");
    return -1;
}

/* --- file level --------------------------------------------------------------------*/

pdf_file *pdf_parse(const uint8_t *buf, size_t len, char *err, size_t errsz)
{
    pdf_file *f = calloc(1, sizeof(*f));
    pdf_lex lx;

    if (!f) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    f->buf = buf;
    f->len = len;
    f->root_num = UINT32_MAX;
    lx.buf = buf;
    lx.len = len;
    lx.pos = 0;
    lx.f = f;

    if (len < 9 || memcmp(buf, "%PDF-", 5) != 0) {
        snprintf(f->error, sizeof(f->error), "missing %%PDF- header");
        goto fail;
    }

    /* startxref: the last one wins (PDF 1.4 has exactly one) */
    size_t sx = 0;
    for (size_t i = len >= 9 ? len - 9 : 0;; i--) {
        if (!memcmp(buf + i, "startxref", 9)) {
            sx = i;
            break;
        }
        if (i == 0)
            break;
    }
    if (!sx) {
        snprintf(f->error, sizeof(f->error), "startxref not found");
        goto fail;
    }
    lx.pos = sx + 9;
    int64_t xref_ofs = 0;

    if (!lex_int(&lx, &xref_ofs) || xref_ofs < 0 ||
        (size_t)xref_ofs >= len) {
        snprintf(f->error, sizeof(f->error), "bad startxref offset");
        goto fail;
    }
    /* whitespace only here: skip_ws would treat the leading '%' of the
     * %%EOF marker as a comment and swallow it */
    while (lx.pos < len && is_ws(buf[lx.pos]))
        lx.pos++;
    if (lx.pos + 5 > len || memcmp(buf + lx.pos, "%%EOF", 5) != 0) {
        snprintf(f->error, sizeof(f->error), "missing %%EOF after startxref");
        goto fail;
    }
    lx.pos += 5;
    while (lx.pos < len && is_ws(buf[lx.pos]))
        lx.pos++;
    if (lx.pos != len) {
        snprintf(f->error, sizeof(f->error), "trailing bytes after %%EOF");
        goto fail;
    }

    /* xref table */
    lx.pos = (size_t)xref_ofs;
    if (!accept_word(&lx, "xref")) {
        snprintf(f->error, sizeof(f->error), "xref table not at startxref");
        goto fail;
    }
    typedef struct {
        uint32_t num, gen;
        size_t   ofs;
    } xent;
    xent *ents = NULL;
    size_t n_ents = 0, cap_ents = 0;
    uint32_t max_num = 0;

    for (;;) {
        char word[16];

        peek_word(&lx, word);
        if (!strcmp(word, "trailer"))
            break;
        int64_t first = 0, count = 0;
        if (!lex_int(&lx, &first) || !lex_int(&lx, &count) || first < 0 ||
            count < 0 || count > 1000000) {
            snprintf(f->error, sizeof(f->error), "bad xref subsection");
            goto fail_ents;
        }
        for (int64_t i = 0; i < count; i++) {
            int64_t ofs = 0, gen = 0;
            char type[16];

            if (!lex_int(&lx, &ofs) || !lex_int(&lx, &gen)) {
                snprintf(f->error, sizeof(f->error), "bad xref entry");
                goto fail_ents;
            }
            skip_word(&lx, type);
            if ((strcmp(type, "n") && strcmp(type, "f")) || ofs > (int64_t)len) {
                snprintf(f->error, sizeof(f->error), "bad xref entry type");
                goto fail_ents;
            }
            if (type[0] != 'n')
                continue;
            if (n_ents == cap_ents) {
                cap_ents = cap_ents ? cap_ents * 2 : 64;
                xent *ne = realloc(ents, cap_ents * sizeof(*ne));
                if (!ne) {
                    snprintf(f->error, sizeof(f->error), "out of memory");
                    goto fail_ents;
                }
                ents = ne;
            }
            uint32_t num = (uint32_t)(first + i);
            ents[n_ents++] = (xent){ num, (uint32_t)gen, (size_t)ofs };
            if (num > max_num)
                max_num = num;
        }
    }
    if (!accept_word(&lx, "trailer")) {
        snprintf(f->error, sizeof(f->error), "trailer missing");
        goto fail_ents;
    }
    pdf_obj *trailer = parse_value(&lx);
    if (!trailer || trailer->kind != PDF_DICT) {
        if (!f->error[0])
            snprintf(f->error, sizeof(f->error), "trailer is not a dictionary");
        goto fail_ents;
    }
    pdf_obj *root = pdf_dict_get(trailer, "Root");
    if (!root || root->kind != PDF_REF) {
        snprintf(f->error, sizeof(f->error), "trailer without /Root ref");
        goto fail_ents;
    }
    f->root_num = (uint32_t)root->u.ref.num;

    /* load every in-use object */
    f->n_objs = max_num + 1;
    f->objs = calloc(f->n_objs, sizeof(*f->objs));
    if (!f->objs) {
        snprintf(f->error, sizeof(f->error), "out of memory");
        goto fail_ents;
    }
    for (size_t i = 0; i < n_ents; i++)
        if (ents[i].num < f->n_objs &&
            load_object(&lx, ents[i].num, ents[i].gen, ents[i].ofs) != 0)
            goto fail_ents;
    free(ents);
    return f;

fail_ents:
    free(ents);
fail:
    snprintf(err, errsz, "%s", f->error[0] ? f->error : "parse error");
    pdf_file_free(f);
    return NULL;
}

void pdf_file_free(pdf_file *f)
{
    if (!f)
        return;
    arena_release(&f->arena);
    free(f->objs);
    free(f);
}

/* --- object helpers ------------------------------------------------------------------*/

pdf_obj *pdf_dict_get(const pdf_obj *dict, const char *key)
{
    if (!dict || dict->kind != PDF_DICT)
        return NULL;
    size_t klen = strlen(key);
    for (uint32_t i = 0; i < dict->u.dict.n; i++) {
        const pdf_obj *k = dict->u.dict.keys[i];
        if (k->kind == PDF_NAME && k->u.str.len == klen &&
            !memcmp(k->u.str.ptr, key, klen))
            return dict->u.dict.vals[i];
    }
    return NULL;
}

int64_t pdf_dict_get_int(const pdf_obj *dict, const char *key, int64_t def)
{
    const pdf_obj *v = pdf_dict_get(dict, key);

    if (!v)
        return def;
    if (v->kind == PDF_INT)
        return v->u.i;
    if (v->kind == PDF_REAL && v->u.d == (int64_t)v->u.d)
        return (int64_t)v->u.d;
    return def;
}

pdf_obj *pdf_array_get(const pdf_obj *arr, uint32_t i)
{
    if (!arr || arr->kind != PDF_ARRAY || i >= arr->u.arr.n)
        return NULL;
    return arr->u.arr.items[i];
}

pdf_obj *pdf_resolve(pdf_file *f, pdf_obj *obj)
{
    if (!obj || !f)
        return NULL;
    if (obj->kind != PDF_REF)
        return obj;
    uint32_t num = (uint32_t)obj->u.ref.num;
    int32_t gen = obj->u.ref.gen;

    if (num >= f->n_objs || !f->objs[num].obj)
        return NULL;
    if (f->objs[num].number != num)
        return NULL;
    (void)gen;
    return f->objs[num].obj;
}
