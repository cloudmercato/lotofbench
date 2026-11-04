# Benchmarks

Human documentation of the LotofBench workload list, what each workload
measures, and how scores are computed.

LotofBench follows the structure of [Geekbench 6](https://www.geekbench.com)
(Primate Labs). This project is **not affiliated with** Primate Labs; scores
are **not comparable** with Geekbench scores (see [Scoring](#scoring)).

## The Geekbench 6 CPU workload list

Geekbench 6 runs 16 CPU workloads, single-core and multi-core, grouped into
Integer and Floating-Point sections (composite weights: Integer 65%,
Floating Point 35%). The official list, with the LotofBench status:

| # | Geekbench 6 workload          | What it exercises                  | LotofBench      |
|---|-------------------------------|------------------------------------|-----------------|
| 1 | File Compression              | LZ4 + ZSTD compress/decompress, SHA-1 verify | **implemented v0.1** |
| 2 | Navigation                    | Dijkstra routing on OpenStreetMap graphs | **implemented v0.2** |
| 3 | HTML5 Browser                 | HTML parse, CSS layout, render     | **implemented v0.3** |
| 4 | PDF Render                    | Vector PDF rendering (PDFium)      | **implemented v0.4** |
| 5 | Photo Library                 | JPEG decode, thumbnails, MobileNet classify + SQLite | **implemented v0.10** |
| 6 | Clang                         | Compiling the Lua interpreter      | **implemented v0.4** |
| 7 | Text Processing               | Regex parsing, Markdown→HTML (Python) | **implemented v0.5** |
| 8 | Asset Compression             | ASTC/BC7/DXT5 texture codecs, Draco mesh | **implemented v0.11** |
| 9 | Object Detection              | MobileNet v1 SSD over photos       | planned |
| 10| Background Blur               | DeepLabV3+ segmentation on video frames | planned |
| 11| Object Remover                | Inpainting (content-aware fill)    | planned |
| 12| Horizon Detection             | Canny edges + Hough transform      | **implemented v0.8** |
| 13| Photo Filter                  | Color/blur filters, crop, scale    | **implemented v0.7** |
| 14| HDR                           | Blending six 16 MP SDR photos      | planned |
| 15| Ray Tracer                    | Blender BMW scene, Embree-style    | **implemented v0.9** |
| 16| Structure from Motion         | 3D geometry from 2D image pairs    | planned |

Geekbench also runs GPU compute workloads; they are out of scope for now.

## Workload 1 — File Compression (implemented)

Geekbench 6 compresses and decompresses the Ruby 3.1.2 source archive
(75 MB, 9,841 files) with LZ4 and ZSTD at high and low compression ratios,
verifies every output file with SHA-1, all inside an in-memory encrypted
file system (working set: 105.5 MB single-core, 418.2 MB multi-core).

LotofBench v0.1 reproduces the shape of that workload, dependency-free:

- **Corpus**: 100 MB (configurable `--size-mb`) of deterministic synthetic
  data — Zipf-distributed pseudo-text words, repeated phrases at assorted
  back-distances, and zero runs. Fixed `--seed` means byte-identical
  corpora on every machine, so scores are comparable across runs. It is
  generated off the clock.
- **Codecs**: a from-scratch LZ4 block-format implementation, run at two
  ratios — `LZ4_LEVEL_FAST` (single-slot hash table, one candidate) and
  `LZ4_LEVEL_HIGH` (chained candidates within the 64 KiB window, deepest
  match). This mirrors Geekbench's "high and low compression ratios"
  without pulling in external libraries. (Divergence: Geekbench 6 pairs
  LZ4 with ZSTD; lotofbench uses LZ4 at two depths instead.)
- **Verification**: every decompressed buffer is SHA-1-checked against the
  source before the run counts — same spirit as Geekbench 6, which
  verifies its codecs with SHA-1. A failed round-trip aborts the run.
- **Cycle**: one run = compress + decompress + verify at both ratios,
  timed as a whole with `CLOCK_MONOTONIC`.

Known v0.1 divergences from Geekbench 6, all deliberate: synthetic corpus
instead of the Ruby archive, no in-memory encrypted file system, fixed
corpus size instead of the 105.5/418.2 MB working sets.

## Workload 2 — Navigation (implemented)

Geekbench 6's Navigation workload plans routes with Dijkstra's algorithm
over OpenStreetMap data. LotofBench v0.2 reproduces the shape of that
workload, dependency-free:

- **Graph**: a deterministic synthetic road network (fixed `--seed`,
  generated off the clock) — a 512×512 lattice of nodes whose streets
  carry an integer travel time of 10 per grid unit ±20% jitter, ~40% of
  cells add a north-east diagonal shortcut, and 2,048 long-range
  "highways" (endpoints at least 256 units apart) run at 2.5× the base
  speed. The lattice alone guarantees connectivity; shortcuts and
  highways give the search real routing decisions. Roads are
  bidirectional, stored as a directed CSR adjacency (~1.26 M directed
  edges; 1,259,432 on the default seed).
- **Routes**: 64 seeded source/target pairs, straight-line distance
  between 300 and 700 grid units. Each route is an early-exit Dijkstra
  from its source: the search stops as soon as the target is settled.
- **Search**: indexed binary min-heap keyed by travel time, with
  decrease-key (no lazy deletion), so the heap stays bounded by the node
  count. All arithmetic is integer, so results are machine-independent.
- **Verification**: inside the timed cycle, every settled route is
  checked before the run counts — the parent chain is walked back to the
  source and re-summed edge by edge (it must reproduce the settled
  travel time exactly, which catches corrupt parents or wrong keys), the
  chain must use real edges, and the settled time must clear a
  straight-line lower bound of 3 time units per grid unit (highways run
  4 × 0.8 = 3.2, rounded down). A broken search aborts the run.
- **Cycle**: one run = the whole route set, timed as a whole with
  `CLOCK_MONOTONIC`.

Known v0.2 divergences from Geekbench 6, all deliberate: synthetic grid
instead of OpenStreetMap data, early-exit point-to-point Dijkstra, no
contraction hierarchies or other speed-up techniques — the workload
measures the plain search.

## Workload 3 — HTML5 Browser (implemented)

Geekbench 6's HTML5 Browser workload opens real-world web pages in a
headless browser: HTML parse, CSS layout, then render of the text and
images (GB6 itself uses Gumbo, litehtml, FreeType, AGG and libjpeg).
LotofBench v0.3 reproduces the shape of that workload, dependency-free:

- **Pages**: 32 seeded synthetic pages (GB6 opens 8 single-core / 32
  multi-core; LotofBench uses 32 for both, split across threads).
  Six archetype families cycle — portal, article, wiki, feed, search
  results, landing page — with paragraphs, inline formatting, links,
  per-page styles, comments, entities (`&amp;` `&lt;` `&#65;`), void
  elements and images. Pages are generated off the clock (the stand-in
  for the network fetch) and are byte-identical for a given seed.
- **Tokenizer**: hand-rolled HTML tokenizer — tags, attributes
  (quoted and unquoted), comments, doctype, entities, void elements,
  implied list-item closes, stray-close tolerance. The tokenizer
  produces a flat DOM with parent/child/next links.
- **Layout**: block flow — margins collapse to explicit block spacing,
  inline content is word-wrapped into text runs, images are placed at
  their seeded `width`/`height` attributes (280–480 px wide for
  photos, 600–640 px for landing heroes, defaults 240×160 when the
  attributes are absent). Everything is integer arithmetic, so layouts
  are machine-independent. The layout result is a display list of
  records: background fills, border strips, word blits and image
  blocks.
- **Render**: a scanline rasterizer paints the display list into an
  800×600 RGBA framebuffer. Text comes from a built-in 5×7 bitmap
  font (no FreeType), images from procedural patterns (photo
  gradients, bar charts, concentric-ring avatars — no libjpeg). Like
  GB6, rendering is a plain overdraw with no occlusion culling.
- **Verification**: inside the timed cycle, every page is parsed,
  laid out and rendered from scratch, and the result is compared
  against an off-clock reference render of the same page: element
  count, display-list record count and SHA-1 digest of the rendered
  framebuffer must all match exactly. A tokenizer bug, a layout
  drift or a rasterizer corruption aborts the run.
- **Cycle**: one run = the whole page set (parse + layout + render of
  all 32 pages), timed as a whole with `CLOCK_MONOTONIC`.

Known v0.3 divergences from Geekbench 6, all deliberate: synthetic
pages instead of archived real sites, block layout only (no CSS
cascade, floats or flexbox), a bitmap font instead of hinted TrueType,
procedural images instead of JPEG/PNG decode, 800×600 instead of
1280×720.

## Workload 4 — PDF Render (implemented)

Geekbench 6's PDF Render workload opens complex PDF documents and
renders them: it pushes four American National Park Service maps
(897 KB to 1.5 MB of vector art — fills, lines and text) through
PDFium — 4 PDFs single-core, 16 PDFs multi-core. LotofBench v0.4
reproduces the shape of that workload, dependency-free:

- **Corpus**: four seeded "park map" PDF documents, byte sizes matched
  to the four NPS maps — a cover page and a map page each, written as
  real PDF 1.4 files (header, catalog, page tree, uncompressed content
  streams, classic xref table, trailer). Pages are US Letter
  (612×792 pt) rendered at 2x (144 dpi) into 1224×1584 RGB
  framebuffers. Fixed `--seed` means byte-identical documents on every
  machine; the generator uses integer quarter-point arithmetic
  throughout, so there is no printf float rounding in the corpus.
- **Map content**: the map pages exercise the features a vector map
  needs — a stroked coordinate grid, filled elevation-contour blobs
  built from beziers, a lake fill, a donut chart (even-odd fill with a
  hole), rotated place labels (a `cm` rotation plus a procedural 4×6
  cell font drawn as filled rectangles), a legend with swatches and a
  mitered scale bar, a dashed trail, and a clipped inset panel
  (`W n`).
- **Parser**: a from-scratch PDF reader that parses the documents
  strictly — objects, references, dictionaries, arrays, streams,
  xref — where anything outside the emitted subset is a parse error.
- **Renderer**: a from-scratch scanline rasterizer with 4×4
  anti-aliasing: bezier flattening, nonzero and even-odd filling,
  stroking (the stroke is built as filled quads with joins, caps and
  dash splitting) and a coverage clip mask, with gray/RGB/CMYK color.
  Like the HTML5 Browser renderer it is plain overdraw — no
  occlusion culling.
- **Verification**: the whole document set is parsed and rendered
  inside every timed cycle. Each content stream must reproduce its
  generator SHA-1 digest, each rendered page must reproduce its
  prepare-time reference framebuffer digest, a set of
  generator-computed probe pixels must match exactly, and the operator
  count must match the generator's count. Any mismatch aborts the run
  instead of scoring.
- **Cycle**: one run = the whole document set (parse + render of
  every page), timed as a whole with `CLOCK_MONOTONIC`. The multi-core
  pass renders 16 documents (4x the set) split across threads.

Known v0.4 divergences from Geekbench 6, all deliberate: four synthetic
park maps instead of archived NPS files, uncompressed content streams
(no Flate), a procedural cell font instead of hinted TrueType outlines,
and 2x (144 dpi) instead of PDFium's exact rasterization — the
throughput metric (device pixels per second) is unaffected because the
pixel count is exact and machine-independent.

## Workload 5 — Photo Library (implemented)

Geekbench 6's Photo Library workload organizes a photo collection: it
decodes each photo, generates thumbnails, classifies it with MobileNet
1.0 for the auto-tag and stores the metadata in SQLite. LotofBench
v0.10 reproduces the shape of that import session, dependency-free:

- **Corpus**: 16 seeded synthetic photos (1280x960 RGB) in eight
  content classes — beach, forest, desert, ocean, snow, volcano,
  meadow, night — that cycle the way a real camera roll does. Each
  photo is a vertical class gradient, a banding texture, 2-3 seeded
  shapes in the class accent color and per-pixel noise, all integer
  arithmetic. Fixed `--seed` means byte-identical corpora on every
  machine. The photos are generated off the clock — the stand-in for
  the card import (and for the JPEG decode Geekbench measures).
- **Thumbnail ladder**: three exact box thumbnails per photo at 1/4,
  1/8 and 1/16 linear scale (320x240, 160x120, 80x60) via the shared
  `img_box_downscale` kernel — the destination pixel is the half-up
  rounded mean of its source block, so every source pixel contributes
  to exactly one destination pixel per thumbnail.
- **Classifier (the MobileNet stand-in)**: a fixed integer conv pass
  over the 1/8 thumbnail — 8 filters (3x3, 8.8 fixed point), ReLU
  clamp, 2x2 max pool, global average pool — yielding an 8-dim feature
  vector. The filters are workload constants (not learned weights):
  three per-channel mean probes, a luma probe, three signed gradient
  probes and a zero-sum Laplacian. The auto-tag is nearest-centroid
  classification against the per-class reference features, computed in
  prepare by taking the clean class background through the exact same
  downscale + conv path (so the reference sits at the center of what
  the classifier sees for clean photos of that class).
- **Verification**: a pass only counts if every thumbnail reproduces
  its prepare-time SHA-1 digest and every tag recovers the planted
  class of its photo (the corpus generator plants it, so a wrong tag
  means the classifier — or the corpus — is broken and the run aborts
  instead of scoring). The 16-photo corpus itself is only accepted if
  all 16 photos classify correctly at prepare time.
- **Record store (the SQLite stand-in)**: one record per ingested
  photo — id, class tag, luma probe, largest thumbnail size, a
  4-byte integrity tag from the thumbnail digest — inserted into a
  per-slice open-addressing hash table. After the ingest, integrity
  queries run inside the timed cycle: every id must be present with
  the expected fields, and the per-class counts and id lists must
  match the planted ground truth. The multi-core pass gives each
  thread its own store over its own slice; threads never share
  writable state.
- **Cycle**: one run = the whole photo set (thumbnail ladder + digest
  verify + conv + tag + ingest + store queries per photo), timed as a
  whole with `CLOCK_MONOTONIC`. The multi-core pass ingests 64 photos
  (4x the set) split across threads.

Known v0.10 divergences from Geekbench 6, all deliberate: synthetic
photos instead of a real photo library, no JPEG codec (the decode
stage is off the clock and the pipeline reads raw RGB), a fixed
8-filter integer conv pass instead of MobileNet v1, and an in-memory
record store instead of SQLite. The metric is photos per second, the
photo-workload analogue of HTML5 Browser's pages/s; the photo geometry
is fixed by the workload constants, so it is machine-independent.

## Workload 6 — Clang (implemented)

Geekbench 6's Clang workload compiles the Lua interpreter sources with
Clang (-O2, to LLVM bitcode). A system compiler cannot be part of a
reproducible benchmark — machine-dependent binaries, version-dependent
codegen — so LotofBench v0.4 reproduces the *shape* of that workload,
dependency-free, with the same honesty rules as the other workloads:

- **Translation units**: 32 seeded synthetic sources (a Lua-flavoured
  subset: locals and globals, arithmetic and string concat, table
  constructors, field/index access, `if`/`while`/`repeat`/`for` with
  `break`, function definitions and calls, `and`/`or`). Six archetype
  families cycle — call graphs, table building, arithmetic kernels,
  string handling, control flow, mixed modules. Sources are generated
  off the clock (the stand-in for checking out the project); a fixed
  seed means byte-identical sources on every machine.
- **Lexer**: hand-rolled — names, decimal integer literals, string
  literals with escapes, every operator of the subset (no floats: a
  digit run followed by `.` is a lexer error).
- **Parser**: recursive descent with the Lua precedence chain (or <
  and < comparisons < concat < additive < multiplicative, concat
  right-associative), building a flat arena AST.
- **Code generator**: stack-machine bytecode — every proto serializes
  to a byte blob (header, constant table, instruction stream, nested
  child protos). Instructions are 4 bytes: opcode, `a`, `b` (u16 LE);
  jumps carry absolute instruction indices and are fixed up on patch;
  short-circuit `and`/`or` compile to DUP + JZ/JNZ-to-consumer + POP.
- **Verification**: inside the timed cycle, every unit is lexed,
  parsed and compiled from scratch, and the emitted bytes are checked
  against an off-clock reference compilation of the same source:
  the SHA-1 of the bytecode blob must match exactly. A lexer, parser
  or codegen drift aborts the run.
- **Cycle**: one run = the whole unit set (full pipeline on all 32
  units), timed as a whole with `CLOCK_MONOTONIC`. The corpus is about
  960k tokens, and a single-core pass takes 150–250 ms on the reference
  machine — long enough to mean something, short enough for many runs.

Known v0.4 divergences from Geekbench 6, all deliberate: a synthetic
Lua-subset corpus instead of the real Lua sources, a from-scratch
bytecode compiler instead of Clang/LLVM (a system compiler would make
scores version- and flag-dependent), and blob hashing instead of
object-file emission.

## Workload 7 — Text Processing (implemented)

Geekbench 6's Text Processing workload runs regex matching and
Markdown→HTML conversion over text documents, in Python 3. A scripted
runtime cannot be part of a reproducible benchmark — interpreter
versions and library drift change the numbers — so LotofBench v0.5
reproduces the *shape* of that pipeline, dependency-free, with the
same honesty rules as the other workloads:

- **Documents**: 32 seeded synthetic Markdown documents (fixed
  `--seed`, generated off the clock), about 1.5 MB of text. Six
  archetype families cycle — API docs, blog posts, changelogs, log
  digests, FAQs and READMEs — with headings, paragraphs, bullet and
  ordered lists, block quotes, fenced code, links, inline code and
  emphasis. A fixed seed means byte-identical documents on every
  machine.
- **Regex engine**: a hand-rolled backtracking VM. Patterns compile
  to bytecode (literal bytes, `.`, `[...]` classes with negation,
  `\d` `\w` `\s` and their uppercase negations, groups, alternation,
  greedy `* + ? {m,n}`, anchors `^ $`) and execute leftmost-first
  over an explicit backtrack stack (bounded, so a runaway pattern is
  a hard error, never a hang; a step budget proportional to text
  length × program size backstops the same guarantee). Semantics
  follow Python bytes-mode `re`: quantifiers are greedy,
  alternation is leftmost-first with branch priority, `.` does not
  match `\n`, negated classes do, and an empty match counts once
  before the scan advances one byte. Malformed patterns are compile
  errors, never silently accepted.
- **Patterns**: each document carries 8 seeded patterns drawn from
  its archetype's signatures (version strings, URLs, e-mails, quoted
  strings, code spans, log severities, identifiers, plain words),
  compiled off the clock.
- **Markdown→HTML**: a from-scratch converter over a documented
  subset — ATX headings, fenced code blocks (verbatim), block quotes,
  unordered and ordered lists, horizontal rules, paragraphs
  (consecutive plain lines joined with one space), and inline `code`,
  **strong**, *em* and [links](url) with `&` `<` `>` escaped
  everywhere. Every block emits its HTML followed by one newline;
  unmatched inline markers stay literal.
- **Verification**: inside the timed cycle, every document is scanned
  by all 8 patterns and converted from scratch, and both results are
  checked against off-clock references: each pattern's match count
  and the SHA-1 digest of the converted HTML must match exactly. A
  regex or conversion drift aborts the run.
- **Cycle**: one run = the whole document set (8 regex scans + one
  conversion per document), timed as a whole with `CLOCK_MONOTONIC`.
  The task is about 13.3 MB (each document counted once for the
  conversion and once per pattern for the scans), and a single-core
  pass takes 100–200 ms on the reference machine — long enough to
  mean something, short enough for many runs.

Known v0.5 divergences from Geekbench 6, all deliberate: synthetic
documents instead of real text corpora, a from-scratch regex engine
over the documented subset instead of Python's `re` module, and a
from-scratch Markdown converter instead of a Python markdown library.

## Workload 8 — Asset Compression (implemented)

Geekbench 6's Asset Compression workload pushes 16 texture and geometry
assets through compression codecs — ASTC, BC7, DXTC for textures and
Draco for meshes. LotofBench v0.11 reproduces the shape dependency-free
with the DXTC family plus a Draco-style geometry codec:

- **Assets**: 16 seeded assets — 8 opaque 256x256 textures (four
  archetype families cycling: banded gradient, 8x8 flat-color mosaic,
  concentric rings, two-tone checker with a diagonal ramp, all with
  per-pixel noise), 4 alpha 256x256 textures (the same families with
  seeded disks and a blocky checker in the alpha channel, the stand-in
  for a sprite sheet) and 4 seeded triangle-strip meshes (3-6 strips of
  16-64 triangles, affine vertex rows — the layout a strip-ordered mesh
  gives a geometry codec). Fixed `--seed` means byte-identical assets
  on every machine; generation is off the clock.
- **Texture codecs (BC1 / BC3, the DXTC family)**: a from-scratch
  block compressor and decoder — 4x4 color blocks, 5-6-5 endpoint
  words, the 4-color palette with round-half-up 1/3 and 2/3
  interpolants. The encoder evaluates bounding-box endpoints at two
  effort levels: `AC_EFFORT_FAST` (raw box) and `AC_EFFORT_HIGH` (three
  additional inset variants, quarter steps of the channel range), first
  candidate winning ties; flat blocks get a +1 endpoint bump so the
  stored color0 word is always greater than color1 (4-color mode).
  Opaque textures run BC1 at both efforts; alpha textures run BC3 (the
  8-byte alpha block with exact min/max endpoints, nearest-of-8 search,
  +1 bump for the 8-interpolant mode, plus a FAST color block). The
  decoder implements the full DXTC rules, including the 3-color
  transparent mode (stored color0 <= color1, index 3 decodes alpha 0)
  that the encoder never emits.
- **Geometry codec (the Draco stand-in)**: positions quantized per
  component to 12 bits over the per-component range (round half up),
  then a stream of zigzag-encoded LEB128 varints — vertex 0 raw,
  vertices 1-2 as deltas vs the previous vertex, later vertices against
  a parallelogram prediction (the previous three vertices), which a
  strip-ordered mesh makes near-exact; the index list is a zigzag
  varint delta chain. The codec is lossless at the quantized level for
  ANY indexed mesh — foreign vertex and triangle orders only enlarge
  residuals (the unit tests prove it on reversed and shuffled orders).
- **Verification**: every timed cycle re-encodes and decodes every
  asset of the pass, and each result is checked against the
  prepare-time references — the SHA-1 of the encoded stream, the SHA-1
  of the decoded artifact (RGBA image, or the decoded quantized
  positions and index list for meshes), and the total squared RGB error
  of the decode vs the source (0 for meshes, whose round-trip is
  exact). Any drift aborts the run instead of scoring.
- **Cycle**: one run = the whole asset set (2 codec passes per opaque
  texture, 1 per alpha texture and mesh), timed as a whole with
  `CLOCK_MONOTONIC`. The multi-core pass runs 64 asset instances (4x
  the set) split across threads.

Known v0.11 divergences from Geekbench 6, all deliberate: the DXTC
family instead of ASTC/BC7 (BC1/BC3 are the simplest fixed-func block
codecs to implement dependency-free and bit-identically), a documented
quantize + parallelogram + zigzag + varint geometry codec instead of
Draco's full mesh-processing pipeline, 256x256 textures instead of
GB6's assorted sizes, and synthetic seeded assets instead of real
texture/mesh files — the throughput metric (asset bytes per second) is
unaffected because the byte count is exact and machine-independent.

## Workload 12 — Horizon Detection (implemented)

Geekbench 6's Horizon Detection workload finds the horizon line in a
set of photos with a Canny edge detector followed by a Hough
transform — the classic "where does the sky meet the ground" computer
vision pass. LotofBench v0.8 reproduces the shape dependency-free,
integer-only:

- **Corpus**: eight seeded synthetic photos (1600×1200 RGB) cycling six
  archetype families — open sky, mountain ridge, sea horizon, desert
  dunes, city skyline, overcast dusk. Each photo is a seeded sky
  gradient, a textured ground half, and a *planted horizon*: a
  piecewise-straight boundary (one straight segment, slope −64..+64,
  midpoint within ±50 px of the frame centre) whose parameters are
  drawn from the seed. Shapes silhouetted on the ground shade the sky
  side; per-pixel noise completes the scene. Fixed `--seed` means
  byte-identical photos on every machine; generation is off the clock.
- **Pipeline**: the full classical chain, every stage a documented
  integer kernel — Rec.601 grayscale (8.8 fixed point), a separable
  5-tap Gaussian blur (`[1 4 6 4 1]/16`, ~1.4 px sigma), 3×3 Sobel
  gradients (`|gx|+|gy|` magnitude with the 2-bit orientation sector
  packed alongside), 3×3 non-maximum suppression (sector-aware
  neighbours, 2-px ridge rule), a double threshold with hysteresis
  (strong `HD_EDGE_HIGH` seeds a bounded DFS through weak
  `HD_EDGE_LOW` neighbours), then the Hough transform over **720
  theta bins of 0.25°** and a ±2001 px rho range (the frame diagonal).
  The peak vote cell above `HD_MIN_VOTES` (300) is the detection.
- **Why 0.25° bins**: at 1° per bin a tilted line's vote ridge spreads
  over several rho cells and a 160 px building edge outvotes the
  horizon; at 0.25° the horizon peak concentrates (~550+ votes on the
  reference corpus) above every distractor (≤ ~134 votes), with the
  trig table rounded once at generation time so every machine
  computes the identical accumulator.
- **Verification**: the whole photo set is detected inside every timed
  cycle. Each detection is checked twice — against the prepare-time
  reference result (bit-identical) *and* against the planted ground
  truth: the detected normal must align with the planted normal within
  1°, rho within 3 px, and clear the vote floor. A Canny or Hough
  drift aborts the run instead of scoring.
- **Cycle**: one run = the whole photo set (all pipeline stages of
  every photo), timed as a whole with `CLOCK_MONOTONIC`. The
  multi-core pass detects 32 photos (4x the set) split across threads.

Known v0.8 divergences from Geekbench 6, all deliberate: synthetic
photos with a planted, verifiable horizon instead of real aerial
imagery, a fixed documented pipeline (GB6 does not publish its exact
Canny/Hough parameters), and the vote-count detection rule in place of
GB6's unspecified peak post-processing — the throughput metric (input
pixels per second) is unaffected because the pixel count is exact and
machine-independent.

## Workload 13 — Photo Filter (implemented)

Geekbench 6's Photo Filter workload runs a stack of image-processing
filters over a set of photos — color/blur filters, levels, crop and
scale — 10 photos single-core, 40 multi-core. LotofBench v0.7
reproduces the shape dependency-free:

- **Corpus**: ten seeded synthetic photos (1600×1200 RGB) cycling six
  archetype families — sunset landscape, studio portrait, night
  street, product shot, food close-up, scanned document. Each photo is
  a vertical gradient, a parabolic banding texture, seeded shapes
  (disks built with an exact integer square root, rectangles) and
  per-pixel noise, all integer arithmetic. Fixed `--seed` means
  byte-identical photos on every machine; generation is off the clock
  (the stand-in for the camera import).
- **Pipeline**: a fixed five-stage filter stack per photo —
  saturation (Rec.601 luma, 8.8 fixed point), a black/white-point
  levels stretch, a two-pass box blur (radius 2, the Gaussian
  approximation), a central 7/8 crop and a bilinear rescale back to
  the full frame. Every stage is a shared integer kernel
  (`src/imgops.c`) that the later image workloads reuse; there is no
  floating point anywhere, so results are bit-identical across
  machines and compilers.
- **Verification**: the whole photo set is filtered inside every timed
  cycle, and each filtered output must reproduce its prepare-time
  reference SHA-1 digest; any mismatch aborts the run instead of
  scoring.
- **Cycle**: one run = the whole photo set (all five stages of every
  photo), timed as a whole with `CLOCK_MONOTONIC`. The multi-core pass
  filters 40 photos (4x the set) split across threads.

Known v0.7 divergences from Geekbench 6, all deliberate: synthetic
photos instead of real image files, a two-pass box blur instead of
GB6's exact blur kernel, and one fixed documented pipeline instead of
GB6's per-photo filter choices — the throughput metric (output pixels
per second) is unaffected because the pixel count is exact and
machine-independent.

## Workload 15 — Ray Tracer (implemented)

Geekbench 6's Ray Tracer workload renders the Blender Foundation's BMW
car scene with a custom ray tracer built on the Intel Embree kernels —
the flagship image-synthesis pass. LotofBench v0.9 reproduces the shape
dependency-free, bit-identically:

- **Scene**: a seeded synthetic showroom — a 16×16-cell checker floor
  and six slots filled with seeded primitives: geodesic spheres (an
  octahedron subdivided twice, smooth radial normals), yaw-rotated
  boxes and 16-segment cylinders, in chrome, glossy-paint, matte and
  floor materials, lit by three seeded point lights (white/warm/cool).
  Every angle enters through the shared integer trig table
  (`src/itrig.h`); the sphere tessellation needs no transcendental at
  all. Fixed `--seed` means a bit-identical scene on every machine;
  generation and the BVH build are off the clock.
- **Cameras**: eight seeded orbit cameras (480×360 viewport) looking at
  the middle of the showroom — position from the trig table, radius and
  height from exact float steps, focal length from a hard-coded tan(30°)
  double. No libm call anywhere on the image path.
- **Pipeline**: Whitted shading over a BVH (median split on the longest
  centroid axis, merge sort tie-broken by index, leaves of at most 4
  triangles, near-child-first traversal) — per hit: ambient + Lambert
  diffuse + Phong specular (integer power loop) with one shadow ray per
  light, plus mirror reflections up to 3 bounces for the chrome, paint
  and floor materials; a piecewise sky/ground-haze gradient fills the
  misses. The timed path is float arithmetic restricted to IEEE-exact
  operations (`+ - * / sqrtf`) under `-ffp-contract=off`, so frames and
  ray counts are bit-identical across machines.
- **Verification**: every timed cycle re-renders the whole view set and
  each frame must reproduce its prepare-time SHA-1 digest AND its exact
  ray count (primary + shadow + reflection rays are all counted). The
  BVH renderer and a brute-force renderer (no BVH) are bit-identical by
  construction — the differential test that validates the acceleration
  structure. A drift aborts the run instead of scoring.
- **Cycle**: one run = the whole view set (all rays of all 8 views),
  timed as a whole with `CLOCK_MONOTONIC`. The multi-core pass renders
  32 views (4x the set) split across threads.

Known v0.9 divergences from Geekbench 6, all deliberate: a synthetic
showroom instead of the licensed BMW mesh (~1k triangles instead of
~140k), one fixed documented shader (ambient/Lambert/Phong + Whitted
mirrors) instead of Embree's unspecified materials, and a pixel-centre
primary ray with no jitter or anti-aliasing — the throughput metric
(rays per second) is unaffected because the ray count is exact and
machine-independent.

## Metric

Each run reports a **throughput**, and the workload result is the
**median** over `--runs` runs (default 5).

- **File Compression** — corpus bytes divided by the wall-clock seconds
  of the full compress + decompress + verify cycle, in decimal
  megabytes per second (1 MB = 1,000,000 bytes). Example ratios on the
  reference corpus: zero runs compress ~158:1, repeated phrases 2-4:1,
  pseudo-text roughly 1.03:1 with the fast level.
- **Navigation** — edge relaxations divided by the wall-clock seconds of
  the full route set, in decimal millions of edges per second (Medge/s).
  An edge relaxation is one scan of one directed adjacency entry by the
  search; the count is exact and deterministic (same seed, same count),
  so it is the navigation analogue of the corpus size. On the default
  seed the suite relaxes 52,954,982 edges per pass.
- **HTML5 Browser** — pages rendered divided by the wall-clock seconds
  of the full page set, in pages per second. A page "render" is the
  whole pipeline (parse + layout + raster), so the unit is honest at
  the workload level even though it compresses three distinct stages.
- **PDF Render** — device pixels rendered divided by the wall-clock
  seconds of the full document set, in millions of pixels per second
  (Mpixel/s). The pixel count is exact (pages are a fixed 1224×1584
  each) and machine-independent, like the corpus size for File
  Compression.
- **Clang** — source tokens compiled divided by the wall-clock seconds
  of the full unit set, in decimal thousands of tokens per second
  (Ktokens/s). A "token compiled" is one lexer token carried through
  the whole pipeline (lex + parse + codegen + verify); the count is
  exact and deterministic (same seed, same count), so it is the Clang
  analogue of the corpus size. On the default seed the suite compiles
  986,328 tokens per pass.
- **Text Processing** — text bytes processed divided by the
  wall-clock seconds of the full document set, in decimal megabytes
  per second (1 MB = 1,000,000 bytes). Each document counts once for
  the Markdown→HTML stage and once per pattern for the regex stage —
  9× the corpus size, exact and deterministic (same seed, same
  count), so it is the text-processing analogue of the corpus size.
  On the default seed the suite processes 13,317,768 bytes per pass.
- **Asset Compression** — asset bytes pushed through the codecs
  divided by the wall-clock seconds of the full asset set, in decimal
  megabytes per second (1 MB = 1,000,000 bytes). Each asset counts its
  exact input bytes: w*h*3 per BC1 pass (2 passes for opaque
  textures), w*h*4 for BC3, vertex_count*12 + index_count*4 for
  meshes — exact and deterministic (same seed, same count), so it is
  the asset-compression analogue of the corpus size. On the default
  seed the suite pushes 4,213,816 bytes per pass.
- **Horizon Detection** — input pixels detected divided by the
  wall-clock seconds of the full photo set, in millions of pixels per
  second (Mpixel/s). Each photo contributes its exact 1600×1200 frame
  (15,360,000 pixels per single-core pass); the count is
  machine-independent, like the corpus size for File Compression.
- **Photo Filter** — output pixels filtered divided by the wall-clock
  seconds of the full photo set, in millions of pixels per second
  (Mpixel/s). Each photo contributes its exact 1600×1200 output frame
  (19,200,000 pixels per single-core pass); the count is
  machine-independent, like the corpus size for File Compression.
- **Ray Tracer** — traced rays divided by the wall-clock seconds of the
  full view set, in millions of rays per second (Mray/s). Every
  primary, shadow and reflection ray is counted exactly; the count is
  deterministic (same seed, same count), so it is the ray tracer's
  analogue of the corpus size. On the default seed the suite traces
  5,690,792 rays per pass.

## Scoring

```
score = round(throughput / baseline * 1000)
```

Baselines are **LotofBench calibration constants**, chosen when each
workload landed, defined in `src/lotofbench.h`. They are not Geekbench
numbers, and **LotofBench scores are not comparable with Geekbench
scores**: Geekbench 6 calibrates its composite at 2500 (Core i7-12700
reference) and folds its 16 workloads into Integer/Floating-Point
subsections with geometric means, then weights them 65/35. LotofBench
will adopt the same aggregation once more workloads exist; with eleven
workloads there is still nothing to aggregate, so each workload
publishes its own score.

- `BASELINE_FILECOMPRESSION_MBPS` = **250.0 MB/s** = 1000 (workload #1).
- `BASELINE_NAVIGATION_MEDGES` = **100.0 Medge/s** = 1000 (workload #2)
  — the reference machine relaxes one edge per nanosecond.
- `BASELINE_HTML5BROWSER_PPS` = **650.0 pages/s** = 1000 (workload #3)
  — the reference machine renders a page (parse + layout + raster of
  an 800×600 viewport) in about 1.5 milliseconds.
- `BASELINE_PDFRENDER_MPIXELS` = **10.0 Mpixel/s** = 1000 (workload #4)
  — the reference machine rasterizes about 10 million device pixels
  per second (a 1224×1584 map page in roughly a fifth of a second,
  parse included).
- `BASELINE_PHOTOLIBRARY_IPS` = **200.0 img/s** = 1000 (workload #5)
  — the reference machine ingests a 1280×960 photo in about five
  milliseconds (thumbnail ladder + conv + auto-tag + store).
- `BASELINE_CLANG_KTOKENS` = **8500.0 Ktokens/s** = 1000 (workload #6)
  — the reference machine compiles about 8.5 million source tokens per
  second (lex + parse + codegen + verify).
- `BASELINE_TEXTPROCESSING_MBPS` = **200.0 MB/s** = 1000 (workload #7)
  — the reference machine processes about 200 million text bytes per
  second (8 regex scans + one Markdown conversion per document).
- `BASELINE_ASSETCOMPRESSION_MBPS` = **50.0 MB/s** = 1000 (workload #8)
  — the reference machine pushes about 50 million asset bytes per
  second (the 16-asset set in about 93 ms single-core).
- `BASELINE_HORIZONDETECTION_MPIXELS` = **50.0 Mpixel/s** = 1000
  (workload #12) — the reference machine runs the full grayscale →
  blur → Sobel → NMS → hysteresis → Hough chain over about 50 million
  pixels per second (a 1600×1200 photo in about 38 ms).
- `BASELINE_PHOTOFILTER_MPIXELS` = **50.0 Mpixel/s** = 1000 (workload
  #13) — the reference machine filters about 50 million output pixels
  per second (the five-stage stack over a 1600×1200 photo in about
  45 ms).
- `BASELINE_RAYTRACER_MRAYS` = **5.0 Mray/s** = 1000 (workload #15) —
  the reference machine traces about 5 million rays per second through
  the BVH (the 8-view pass in about 1.1 s single-core).

## Multi-core model

Geekbench 6 moved to a **shared task** model: threads cooperate on one
task, each handling a slice, instead of running independent copies.
LotofBench does the same for every workload:

- **File Compression** — the corpus is split into equal 64-byte-aligned
  chunks, one per thread.
- **Navigation** — the route set is split into equal contiguous slices,
  one per thread.
- **HTML5 Browser** — the page set is split into equal contiguous
  slices, one per thread; each thread tokenizes, lays out and rasters
  its pages, and verifies them against the reference render.
- **PDF Render** — the multi-core pass renders 16 documents (4x the
  single-core set) split into equal contiguous document slices, one per
  thread; each thread parses, renders and verifies its documents
  against the references.
- **Clang** — the unit set is split into equal contiguous slices, one
  per thread; each thread lexes, parses, compiles and verifies its
  units against the reference blobs.
- **Text Processing** — the document set is split into equal
  contiguous slices, one per thread; each thread scans its documents'
  8 patterns, converts them Markdown→HTML and verifies both against
  the references.
- **Asset Compression** — the multi-core pass runs 64 asset instances
  (4x the single-core set) split into equal contiguous slices, one per
  thread; each thread runs every codec pass of its assets (BC1 fast +
  high, BC3, geometry) and verifies the results against the
  references.
- **Horizon Detection** — the multi-core pass detects 32 photos (4x
  the single-core set) split into equal contiguous slices, one per
  thread; each thread runs the full Canny + Hough chain on its photos
  and verifies every detection against the reference results and the
  planted ground truth.
- **Ray Tracer** — the multi-core pass renders 32 views (4x the
  single-core set) split into equal contiguous slices, one per thread;
  each thread traces its views through the BVH and verifies every
  frame's digest and ray count against the references.
- **Photo Filter** — the multi-core pass filters 40 photos (4x the
  single-core set) split into equal contiguous slices, one per thread;
  each thread runs all five stages on its photos and verifies every
  output against the reference digests.

Runs are synchronised through a start gate (every thread must be ready
before the clock starts, and the clock stops when the last thread
finishes), so elapsed time measures the whole task.

Default thread count: every online CPU (capped at 64), override with
`--threads`. Single-core is always one thread over the whole task.
