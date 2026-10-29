# Graph Report - lotofbench  (2026-10-29)

## Corpus Check
- 36 files · ~54,127 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 5 file(s) not represented in the graph (top: (none) 5)

## Summary
- 513 nodes · 1853 edges · 17 communities (14 shown, 3 thin omitted)
- Extraction: 79% EXTRACTED · 21% INFERRED · 0% AMBIGUOUS · INFERRED: 390 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `012146a9`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- tests.h
- workload_navigation.c
- harness.c
- test_workload_clang.c
- main.c
- lz4.c
- workload_clang.c
- fc_prepare
- workload_html5browser.c
- string
- cl_scratch
- Benchmarks
- gen_page
- workload_compression.c
- workload_textprocessing.c
- main

## God Nodes (most connected - your core abstractions)
1. `rng_below()` - 49 edges
2. `main()` - 38 edges
3. `test_fail()` - 27 edges
4. `buf_putc()` - 25 edges
5. `gen_expr()` - 23 edges
6. `buf_puts()` - 21 edges
7. `main()` - 20 edges
8. `gen_stmt()` - 19 edges
9. `rng_init()` - 17 edges
10. `buf_putn()` - 17 edges

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

## Communities (17 total, 3 thin omitted)

### Community 1 - "workload_navigation.c"
Cohesion: 0.13
Nodes (35): grid_d2(), nav_cycle(), nav_dests(), nav_edge_count(), nav_free(), nav_heap_pop(), nav_heap_push(), nav_nodes() (+27 more)

### Community 2 - "harness.c"
Cohesion: 0.16
Nodes (9): harness_run(), lb_median(), now_sec(), worker(), test_harness_counts_and_times(), test_harness_failure_propagates(), test_harness_single_thread(), test_median_odd_even() (+1 more)

### Community 3 - "test_workload_clang.c"
Cohesion: 0.16
Nodes (34): cl_compile(), cl_free(), cl_lex_count(), cl_parse_count(), cl_prepare(), cl_run(), cl_tokens(), cl_unit_blob() (+26 more)

### Community 4 - "main.c"
Cohesion: 0.19
Nodes (11): lb_score(), main(), parse_args(), print_block(), print_json(), run_cl(), run_fc(), run_h5() (+3 more)

### Community 5 - "lz4.c"
Cohesion: 0.12
Nodes (24): emit_last_literals(), hash32(), literal_only_bytes(), lz4_compress(), lz4_compress_bound(), lz4_decompress(), lz4_scratch_get(), match_len() (+16 more)

### Community 6 - "workload_clang.c"
Cohesion: 0.26
Nodes (42): rng_below(), buf_grow(), buf_putc(), buf_putn(), buf_puts(), buf_putu(), buf_putu16(), buf_putu64() (+34 more)

### Community 7 - "fc_prepare"
Cohesion: 0.31
Nodes (10): fc_corpus(), fc_free(), fc_len(), fc_prepare(), fc_run(), fill_corpus(), test_fc_accessors_null_safe(), test_fc_corpus_golden() (+2 more)

### Community 8 - "workload_html5browser.c"
Cohesion: 0.08
Nodes (63): attr_number(), class_lookup(), decode_entity(), emit_words(), ensure_fb(), fill_rect(), find_body(), font_advance() (+55 more)

### Community 9 - "string"
Cohesion: 0.26
Nodes (7): rol32(), sha1(), sha1_block(), sha1_hex(), cl_cycle(), test_sha1_known_vectors(), test_sha1_million_a()

### Community 10 - "cl_scratch"
Cohesion: 0.13
Nodes (51): binop_of(), binop_prec(), block_ender(), cg_block(), cg_expr(), cg_frame(), cg_proto(), cg_stmt() (+43 more)

### Community 11 - "Benchmarks"
Cohesion: 0.06
Nodes (29): Benchmarks, Metric, Multi-core model, Scoring, The Geekbench 6 CPU workload list, Workload 1 — File Compression (implemented), Workload 2 — Navigation (implemented), Workload 3 — HTML5 Browser (implemented) (+21 more)

### Community 12 - "gen_page"
Cohesion: 0.58
Nodes (11): buf_grow(), buf_putc(), buf_putn(), buf_puts(), buf_putu(), gen_img(), gen_page(), gen_par() (+3 more)

### Community 15 - "workload_textprocessing.c"
Cohesion: 0.14
Nodes (48): buf_grow(), buf_putc(), buf_putn(), buf_puts(), buf_putu(), buf_reset(), build_words(), class_item() (+40 more)

### Community 16 - "main"
Cohesion: 0.24
Nodes (26): machine_free(), tp_bytes(), tp_doc_digest(), tp_doc_html(), tp_doc_html_len(), tp_doc_len(), tp_doc_matches(), tp_doc_pattern() (+18 more)

## Knowledge Gaps
- **3 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `rng_below()` connect `workload_clang.c` to `workload_navigation.c`, `lz4.c`, `fc_prepare`, `gen_page`, `workload_textprocessing.c`, `main`?**
  _High betweenness centrality (0.080) - this node is a cross-community bridge._
- **Are the 46 inferred relationships involving `rng_below()` (e.g. with `gen_anonfunc()` and `gen_binop()`) actually correct?**
  _`rng_below()` has 46 INFERRED edges - model-reasoned connections that need verification._
- **Should `workload_navigation.c` be split into smaller, more focused modules?**
  _Cohesion score 0.12626262626262627 - nodes in this community are weakly interconnected._
- **Why does `main()` connect `main` to `workload_navigation.c`, `test_workload_clang.c`, `lz4.c`, `workload_clang.c`, `fc_prepare`, `workload_html5browser.c`, `string`, `gen_goldens.c`, `workload_textprocessing.c`?**
  _High betweenness centrality (0.080) - this node is a cross-community bridge._
- **Are the 37 inferred relationships involving `main()` (e.g. with `lz4_compress()` and `rng_below()`) actually correct?**
  _`main()` has 37 INFERRED edges - model-reasoned connections that need verification._
- **Should `lz4.c` be split into smaller, more focused modules?**
  _Cohesion score 0.11764705882352941 - nodes in this community are weakly interconnected._
- **Why does `test_fail()` connect `workload_navigation.c` to `tests.h`, `test_workload_clang.c`, `lz4.c`, `fc_prepare`, `workload_html5browser.c`, `string`, `main`?**
  _High betweenness centrality (0.031) - this node is a cross-community bridge._