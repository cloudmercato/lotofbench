# Graph Report - lotofbench  (2026-10-29)

## Corpus Check
- 33 files · ~43,859 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 5 file(s) not represented in the graph (top: (none) 5)

## Summary
- 426 nodes · 1494 edges · 15 communities (13 shown, 2 thin omitted)
- Extraction: 79% EXTRACTED · 21% INFERRED · 0% AMBIGUOUS · INFERRED: 308 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `012146a9`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- string
- workload_navigation.c
- harness.c
- main
- main.c
- lz4.c
- rng_below
- workload_compression.c
- workload_html5browser.c
- sha1_hex
- workload_clang.c
- Benchmarks
- gen_page
- harness_run

## God Nodes (most connected - your core abstractions)
1. `rng_below()` - 38 edges
2. `main()` - 29 edges
3. `buf_putc()` - 25 edges
4. `test_fail()` - 25 edges
5. `gen_expr()` - 23 edges
6. `buf_puts()` - 21 edges
7. `gen_stmt()` - 19 edges
8. `main()` - 17 edges
9. `buf_putn()` - 17 edges
10. `p_stmt()` - 17 edges

## Surprising Connections (you probably didn't know these)
- `test_score_rounding()` --calls--> `lb_score()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_median_odd_even()` --calls--> `lb_median()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_harness_counts_and_times()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_harness_failure_propagates()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_harness_single_thread()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c

## Import Cycles
- None detected.

## Communities (15 total, 2 thin omitted)

### Community 1 - "workload_navigation.c"
Cohesion: 0.13
Nodes (34): grid_d2(), nav_cycle(), nav_dests(), nav_edge_count(), nav_free(), nav_heap_pop(), nav_heap_push(), nav_nodes() (+26 more)

### Community 2 - "harness.c"
Cohesion: 0.22
Nodes (4): lb_median(), now_sec(), worker(), test_median_odd_even()

### Community 3 - "main"
Cohesion: 0.17
Nodes (35): cl_compile(), cl_free(), cl_lex_count(), cl_parse_count(), cl_prepare(), cl_run(), cl_tokens(), cl_unit_blob() (+27 more)

### Community 4 - "main.c"
Cohesion: 0.20
Nodes (10): lb_score(), main(), parse_args(), print_block(), print_json(), run_cl(), run_fc(), run_h5() (+2 more)

### Community 5 - "lz4.c"
Cohesion: 0.12
Nodes (24): emit_last_literals(), hash32(), literal_only_bytes(), lz4_compress(), lz4_compress_bound(), lz4_decompress(), lz4_scratch_get(), match_len() (+16 more)

### Community 6 - "rng_below"
Cohesion: 0.25
Nodes (40): rng_below(), buf_grow(), buf_putc(), buf_putn(), buf_puts(), buf_putu(), buf_putu64(), gen_anonfunc() (+32 more)

### Community 7 - "workload_compression.c"
Cohesion: 0.30
Nodes (11): build_letter_table(), fc_corpus(), fc_free(), fc_len(), fc_prepare(), fc_run(), fill_corpus(), test_fc_accessors_null_safe() (+3 more)

### Community 8 - "workload_html5browser.c"
Cohesion: 0.08
Nodes (63): attr_number(), class_lookup(), decode_entity(), emit_words(), ensure_fb(), fill_rect(), find_body(), font_advance() (+55 more)

### Community 9 - "sha1_hex"
Cohesion: 0.39
Nodes (6): rol32(), sha1(), sha1_block(), sha1_hex(), test_sha1_known_vectors(), test_sha1_million_a()

### Community 10 - "workload_clang.c"
Cohesion: 0.14
Nodes (55): binop_of(), binop_prec(), block_ender(), buf_putu16(), cg_block(), cg_expr(), cg_frame(), cg_proto() (+47 more)

### Community 11 - "Benchmarks"
Cohesion: 0.07
Nodes (28): Benchmarks, Metric, Multi-core model, Scoring, The Geekbench 6 CPU workload list, Workload 1 — File Compression (implemented), Workload 2 — Navigation (implemented), Workload 3 — HTML5 Browser (implemented) (+20 more)

### Community 12 - "gen_page"
Cohesion: 0.58
Nodes (11): buf_grow(), buf_putc(), buf_putn(), buf_puts(), buf_putu(), gen_img(), gen_page(), gen_par() (+3 more)

### Community 13 - "harness_run"
Cohesion: 0.28
Nodes (5): harness_run(), test_harness_counts_and_times(), test_harness_failure_propagates(), test_harness_single_thread(), test_score_rounding()

## Knowledge Gaps
- **2 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `main()` connect `main` to `string`, `workload_navigation.c`, `lz4.c`, `rng_below`, `workload_compression.c`, `workload_html5browser.c`, `sha1_hex`?**
  _High betweenness centrality (0.066) - this node is a cross-community bridge._
- **Are the 35 inferred relationships involving `rng_below()` (e.g. with `gen_anonfunc()` and `gen_binop()`) actually correct?**
  _`rng_below()` has 35 INFERRED edges - model-reasoned connections that need verification._
- **Should `workload_navigation.c` be split into smaller, more focused modules?**
  _Cohesion score 0.12790697674418605 - nodes in this community are weakly interconnected._
- **Why does `rng_below()` connect `rng_below` to `workload_navigation.c`, `main`, `lz4.c`, `workload_compression.c`, `gen_page`?**
  _High betweenness centrality (0.062) - this node is a cross-community bridge._
- **Are the 28 inferred relationships involving `main()` (e.g. with `lz4_compress()` and `rng_below()`) actually correct?**
  _`main()` has 28 INFERRED edges - model-reasoned connections that need verification._
- **Should `lz4.c` be split into smaller, more focused modules?**
  _Cohesion score 0.11764705882352941 - nodes in this community are weakly interconnected._
- **Why does `test_fail()` connect `main` to `string`, `workload_navigation.c`, `lz4.c`, `workload_compression.c`, `workload_html5browser.c`, `sha1_hex`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._