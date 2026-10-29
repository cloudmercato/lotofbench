# Graph Report - lotofbench  (2026-10-29)

## Corpus Check
- Corpus is ~14,050 words - fits in a single context window. You may not need a graph.

## Summary
- 146 nodes · 400 edges · 11 communities (9 shown, 2 thin omitted)
- Extraction: 70% EXTRACTED · 30% INFERRED · 0% AMBIGUOUS · INFERRED: 122 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- Core Headers & Compression
- Navigation Routing
- Harness Timing
- Navigation Tests
- CLI & Reporting
- LZ4 Codec
- RNG Engine
- Frequency Corpus
- Nav CSR Graph
- SHA-1 Hashing
- Reference Heap

## God Nodes (most connected - your core abstractions)
1. `lz4_compress()` - 13 edges
2. `rng_init()` - 13 edges
3. `main()` - 13 edges
4. `nav_free()` - 12 edges
5. `build()` - 12 edges
6. `nav_run()` - 11 edges
7. `test_fail()` - 11 edges
8. `main()` - 10 edges
9. `rng_next()` - 10 edges
10. `rng_below()` - 10 edges

## Surprising Connections (you probably didn't know these)
- `test_harness_counts_and_times()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_harness_failure_propagates()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_harness_single_thread()` --calls--> `harness_run()`  [INFERRED]
  tests/test_harness.c → src/harness.c
- `test_lz4_golden_vectors()` --calls--> `lz4_compress()`  [INFERRED]
  tests/test_lz4.c → src/lz4.c
- `main()` --calls--> `lz4_compress()`  [INFERRED]
  tools/gen_goldens.c → src/lz4.c

## Import Cycles
- None detected.

## Communities (11 total, 2 thin omitted)

### Community 1 - "Navigation Routing"
Cohesion: 0.26
Nodes (13): grid_d2(), nav_cycle(), nav_heap_pop(), nav_heap_push(), nav_prepare(), nav_route(), nav_route_time(), nav_sift_down() (+5 more)

### Community 2 - "Harness Timing"
Cohesion: 0.17
Nodes (8): harness_run(), lb_median(), now_sec(), worker(), test_harness_counts_and_times(), test_harness_failure_propagates(), test_harness_single_thread(), test_median_odd_even()

### Community 3 - "Navigation Tests"
Cohesion: 0.30
Nodes (12): nav_free(), nav_route_pair(), nav_run(), test_fail(), build(), grid_d2(), test_nav_differential_dijkstra(), test_nav_edges_thread_independent() (+4 more)

### Community 4 - "CLI & Reporting"
Cohesion: 0.20
Nodes (9): lb_score(), main(), parse_args(), print_block(), print_json(), run_fc(), run_nav(), usage() (+1 more)

### Community 5 - "LZ4 Codec"
Cohesion: 0.27
Nodes (13): emit_last_literals(), hash32(), literal_only_bytes(), lz4_compress(), lz4_compress_bound(), lz4_decompress(), match_len(), read32() (+5 more)

### Community 6 - "RNG Engine"
Cohesion: 0.36
Nodes (10): rng_below(), rng_init(), rng_next(), test_lz4_golden_vectors(), test_lz4_roundtrip_shapes(), test_rng_below_bounds(), test_rng_deterministic(), test_rng_distribution() (+2 more)

### Community 7 - "Frequency Corpus"
Cohesion: 0.27
Nodes (11): build_letter_table(), fc_corpus(), fc_free(), fc_len(), fc_prepare(), fc_run(), fill_corpus(), test_fc_accessors_null_safe() (+3 more)

### Community 8 - "Nav CSR Graph"
Cohesion: 0.53
Nodes (8): nav_dests(), nav_edge_count(), nav_nodes(), nav_offsets(), nav_weights(), ref_dijkstra(), test_nav_csr_invariants(), test_nav_graph_deterministic()

### Community 9 - "SHA-1 Hashing"
Cohesion: 0.48
Nodes (6): rol32(), sha1(), sha1_block(), sha1_hex(), test_sha1_known_vectors(), test_sha1_million_a()

## Knowledge Gaps
- **2 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `main()` connect `RNG Engine` to `Core Headers & Compression`, `Navigation Routing`, `Navigation Tests`, `LZ4 Codec`, `Frequency Corpus`, `SHA-1 Hashing`?**
  _High betweenness centrality (0.086) - this node is a cross-community bridge._
- **Are the 6 inferred relationships involving `lz4_compress()` (e.g. with `fc_cycle()` and `roundtrip_ok()`) actually correct?**
  _`lz4_compress()` has 6 INFERRED edges - model-reasoned connections that need verification._
- **Why does `nav_run()` connect `Navigation Tests` to `Navigation Routing`, `Harness Timing`, `CLI & Reporting`, `RNG Engine`, `Nav CSR Graph`?**
  _High betweenness centrality (0.065) - this node is a cross-community bridge._
- **Are the 10 inferred relationships involving `rng_init()` (e.g. with `fc_prepare()` and `nav_prepare()`) actually correct?**
  _`rng_init()` has 10 INFERRED edges - model-reasoned connections that need verification._
- **Why does `nav_prepare()` connect `Navigation Routing` to `Nav CSR Graph`, `Navigation Tests`, `CLI & Reporting`, `RNG Engine`?**
  _High betweenness centrality (0.057) - this node is a cross-community bridge._
- **Are the 12 inferred relationships involving `main()` (e.g. with `lz4_compress()` and `rng_below()`) actually correct?**
  _`main()` has 12 INFERRED edges - model-reasoned connections that need verification._
- **Are the 10 inferred relationships involving `nav_free()` (e.g. with `main()` and `test_nav_csr_invariants()`) actually correct?**
  _`nav_free()` has 10 INFERRED edges - model-reasoned connections that need verification._