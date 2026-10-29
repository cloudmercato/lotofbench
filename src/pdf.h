#ifndef LOTOFBENCH_PDF_H
#define LOTOFBENCH_PDF_H

/* LotofBench — internal PDF object model shared by the reader
 * (pdfparse.c), the renderer (pdfrender.c) and the corpus generator
 * (workload_pdfrender.c). Not part of any public API.
 *
 * The parser supports the PDF 1.4 subset the generator emits — and
 * rejects everything else strictly, so a parsing bug can never silently
 * score. Supported: classic xref tables (no xref streams), indirect
 * objects, arrays, dicts, names, numbers, strings (skipped), streams
 * with direct /Length, and the page tree down to /Contents. */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PDF_NULL, PDF_BOOL, PDF_INT, PDF_REAL, PDF_NAME, PDF_STRING,
    PDF_REF, PDF_ARRAY, PDF_DICT
} pdf_obj_kind;

typedef struct pdf_obj pdf_obj;
struct pdf_obj {
    pdf_obj_kind kind;
    union {
        int         b;
        int64_t     i;
        double      d;
        struct {
            const uint8_t *ptr;
            uint32_t       len;
        } str;                                   /* PDF_NAME / PDF_STRING */
        struct { int32_t num, gen; } ref;
        struct { pdf_obj **items; uint32_t n; } arr;
        struct { pdf_obj **keys; pdf_obj **vals; uint32_t n; } dict;
    } u;
};

typedef struct {
    pdf_obj   *dict;   /* stream dictionary (PDF_DICT) */
    const uint8_t *data;
    uint32_t   len;
} pdf_stream;

typedef struct {
    uint32_t    number;
    pdf_obj    *obj;    /* NULL for free slots */
    pdf_stream *stream; /* non-NULL when the object carries a stream */
} pdf_entry;

/* Allocation arena for parsed objects. */
typedef struct {
    void  **slots;
    size_t  n, cap;
} pdf_arena;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    pdf_entry     *objs;      /* indexed by object number */
    uint32_t       n_objs;    /* capacity actually allocated */
    uint32_t       root_num;  /* /Root object number, UINT32_MAX if absent */
    pdf_arena      arena;
    char           error[128];
} pdf_file;

/* Parse a whole PDF byte buffer. Returns NULL on any structural error
 * (reason copied into err) — strictness is the contract. */
pdf_file *pdf_parse(const uint8_t *buf, size_t len, char *err, size_t errsz);
void      pdf_file_free(pdf_file *f);

/* Dict lookup by literal key (no # escapes). NULL when absent. */
pdf_obj *pdf_dict_get(const pdf_obj *dict, const char *key);

/* Dict lookup as int64 (PDF_INT or integral PDF_REAL); `def` if absent
 * or not numeric. */
int64_t pdf_dict_get_int(const pdf_obj *dict, const char *key, int64_t def);

/* Array item i, NULL when out of range. */
pdf_obj *pdf_array_get(const pdf_obj *arr, uint32_t i);

/* Resolve one `num gen R` reference against the file's object map.
 * Returns the referenced object (or the argument when it is not a
 * reference); NULL when the reference dangles. */
pdf_obj *pdf_resolve(pdf_file *f, pdf_obj *obj);

#endif /* LOTOFBENCH_PDF_H */
