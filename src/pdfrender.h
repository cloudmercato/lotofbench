#ifndef LOTOFBENCH_PDFRENDER_H
#define LOTOFBENCH_PDFRENDER_H

/* LotofBench — internal renderer API (workload #4). Interprets a page's
 * content stream and rasterizes it into an RGB framebuffer. Not part of
 * any public API. */

#include "pdf.h"

/* Device scale: user-space units (1/72 inch) to device pixels.
 * 2.0 = 144 dpi on the 612x792 pt page => 1224 x 1584 pixels. */
#define PDF_RENDER_SCALE 2.0

/* Fixed-point device coordinates: 24.8, one unit = 1/256 pixel. All
 * scan-conversion arithmetic is integer and therefore bit-exact on
 * every machine; the only floating point left is the CTM algebra,
 * compiled with -ffp-contract=off so the transforms are reproducible
 * across compilers. */
#define PDF_FIX 8
#define PDF_ONE (1 << PDF_FIX)
typedef int32_t pdf_fix;

typedef struct {
    uint32_t w, h;
    uint8_t *px; /* RGB24, w * h * 3 bytes */
} pdf_bitmap;

/* Debug capture of the paint command stream, used by the differential
 * unit test: every fill/stroke/clip with its flattened device-space
 * geometry, in paint order. */
typedef struct { pdf_fix x, y; } pdf_pt;
typedef struct { pdf_pt *pts; uint32_t n, cap; int closed; } pdf_contour;
typedef struct { pdf_contour *c; uint32_t n, cap; } pdf_poly;

enum {
    PDF_CMD_FILL_NZ,
    PDF_CMD_FILL_EO,
    PDF_CMD_STROKE,
    PDF_CMD_CLIP
};

typedef struct {
    int      kind;
    uint8_t  r, g, b;
    pdf_poly path;
} pdf_cmd;

typedef struct {
    pdf_cmd *cmds;
    uint32_t n, cap;
} pdf_dbg;

/* Render `clen` bytes of content stream into fb (caller allocates via
 * pdf_bitmap_alloc). media_box = { x0, y0, x1, y1 } in user space.
 * Fills *ops with the exact count of path/paint/clip operators
 * executed. Returns 0, or -1 with a reason in err. */
int pdf_render_contents(const uint8_t *contents, size_t clen,
                        const double media_box[4], pdf_bitmap *fb,
                        uint64_t *ops, char *err, size_t errsz);

/* Same, plus a debug capture of the paint command stream. dbg may be
 * NULL to skip capture. */
int pdf_render_contents_dbg(const uint8_t *contents, size_t clen,
                            const double media_box[4], pdf_bitmap *fb,
                            uint64_t *ops, pdf_dbg *dbg,
                            char *err, size_t errsz);

/* Allocate/free a framebuffer of the given size. */
int  pdf_bitmap_alloc(pdf_bitmap *fb, uint32_t w, uint32_t h);
void pdf_bitmap_free(pdf_bitmap *fb);
void pdf_dbg_free(pdf_dbg *dbg);

#endif /* LOTOFBENCH_PDFRENDER_H */
