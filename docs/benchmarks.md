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
| 4 | PDF Render                    | Vector PDF rendering (PDFium)      | planned |
| 5 | Photo Library                 | JPEG decode, thumbnails, MobileNet classify + SQLite | planned |
| 6 | Clang                         | Compiling the Lua interpreter      | **implemented v0.4** |
| 7 | Text Processing               | Regex parsing, Markdown→HTML (Python) | **implemented v0.5** |
| 8 | Asset Compression             | ASTC/BC7/DXT5 texture codecs, Draco mesh | planned |
| 9 | Object Detection              | MobileNet v1 SSD over photos       | planned |
| 10| Background Blur               | DeepLabV3+ segmentation on video frames | planned |
| 11| Object Remover                | Inpainting (content-aware fill)    | planned |
| 12| Horizon Detection             | Canny edges + Hough transform      | planned |
| 13| Photo Filter                  | Color/blur filters, crop, scale    | planned |
| 14| HDR                           | Blending six 16 MP SDR photos      | planned |
| 15| Ray Tracer                    | Blender BMW scene, Embree-style    | planned |
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
will adopt the same aggregation once more workloads exist; with five
workloads there is still nothing to aggregate, so each workload
publishes its own score.

- `BASELINE_FILECOMPRESSION_MBPS` = **250.0 MB/s** = 1000 (workload #1).
- `BASELINE_NAVIGATION_MEDGES` = **100.0 Medge/s** = 1000 (workload #2)
  — the reference machine relaxes one edge per nanosecond.
- `BASELINE_HTML5BROWSER_PPS` = **650.0 pages/s** = 1000 (workload #3)
  — the reference machine renders a page (parse + layout + raster of
  an 800×600 viewport) in about 1.5 milliseconds.
- `BASELINE_CLANG_KTOKENS` = **8500.0 Ktokens/s** = 1000 (workload #6)
  — the reference machine compiles about 8.5 million source tokens per
  second (lex + parse + codegen + verify).
- `BASELINE_TEXTPROCESSING_MBPS` = **200.0 MB/s** = 1000 (workload #7)
  — the reference machine processes about 200 million text bytes per
  second (8 regex scans + one Markdown conversion per document).

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
- **Clang** — the unit set is split into equal contiguous slices, one
  per thread; each thread lexes, parses, compiles and verifies its
  units against the reference blobs.
- **Text Processing** — the document set is split into equal
  contiguous slices, one per thread; each thread scans its documents'
  8 patterns, converts them Markdown→HTML and verifies both against
  the references.

Runs are synchronised through a start gate (every thread must be ready
before the clock starts, and the clock stops when the last thread
finishes), so elapsed time measures the whole task.

Default thread count: every online CPU (capped at 64), override with
`--threads`. Single-core is always one thread over the whole task.
