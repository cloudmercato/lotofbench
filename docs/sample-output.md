# Sample output

A captured default run of LotofBench 1.0.2 on the development box
(12 online threads, `./lotofbench --json-out docs/sample-output.json`,
2026-11-15, background load ~3 of 12 threads at capture — the box's own
services). The text report is the tool's stdout, verbatim — the
hardware report comes first, then the suite; the same run's
machine-readable results are kept in
[sample-output.json](sample-output.json). Regenerate both files with
the same command after a deliberate output change.

```
LotofBench 1.0.2
seed 1, 5 runs, median

Hardware
  os           : Linux 7.0.0-42-generic (x86_64)
  compiler     : gcc 15.2.0 (x86_64)
  cpu          : AMD Ryzen 5 7640HS w/ Radeon 760M Graphics
  cpu details  : AuthenticAMD, family 25, model 4, stepping 1
  topology     : 1 socket(s), 6 core(s), 12 thread(s)
  frequency    : 5018 MHz
  caches       : L2 1024 KB L3 16384 KB
  memory       : 25327 MB total, 11072 MB free
  swap         : 65535 MB total, 49847 MB free
  numa         : 1 node(s) (node0: 0-11 (25327 MB))
  load average : 3.13 (1m)

File Compression (corpus 100 MB synthetic)
Single-Core
  throughput      107.9 MB/s
  score             432   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      669.4 MB/s
  score            2678

Navigation (grid 512x512, 64 routes)
Single-Core
  throughput       45.1 Medge/s
  score             451   (baseline 100.0 Medge/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      238.3 Medge/s
  score            2383

HTML5 Browser (32 pages, 800x600 viewport, 3544 DOM elements)
Single-Core
  throughput      297.0 pages/s
  score             457   (baseline 650.0 pages/s = 1000)

Multi-Core (12 threads, shared task)
  throughput     2368.6 pages/s
  score            3644

PDF Render (4 park maps, 2 pages each, 1224x1584 px @2x)
Single-Core
  throughput        3.1 Mpixel/s
  score             313   (baseline 10.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       14.2 Mpixel/s
  score            1422

Photo Library (16 photos, 1280x960 px, 8 classes)
Single-Core
  throughput      196.9 img/s
  score             985   (baseline 200.0 img/s = 1000)

Multi-Core (12 threads, shared task)
  throughput     1008.8 img/s
  score            5044

Clang (32 units, 986328 source tokens)
Single-Core
  throughput     4823.2 Ktokens/s
  score             567   (baseline 8500.0 Ktokens/s = 1000)

Multi-Core (12 threads, shared task)
  throughput    26801.8 Ktokens/s
  score            3153

Text Processing (32 docs, 8 patterns/doc)
Single-Core
  throughput       97.1 MB/s
  score             485   (baseline 200.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      494.6 MB/s
  score            2473

Asset Compression (16 assets (12 textures, 4 meshes))
Single-Core
  throughput       46.5 MB/s
  score             930   (baseline 50.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      227.9 MB/s
  score            4558

Object Detection (8 photos, 300x300 px, int8 convnet, SSD-lite head)
Single-Core
  throughput       20.6 img/s
  score             822   (baseline 25.0 img/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      109.0 img/s
  score            4358

Background Blur (12 frames, 1920x1080 px, mask + blur composite)
Single-Core
  throughput       15.6 fps
  score             312   (baseline 50.0 fps = 1000)

Multi-Core (12 threads, shared task)
  throughput       92.5 fps
  score            1851

Object Remover (4 photos, 1600x1200 px, push-pull inpainting)
Single-Core
  throughput       33.9 Mpixel/s
  score            1129   (baseline 30.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      178.6 Mpixel/s
  score            5954

Horizon Detection (8 photos, 1600x1200 px, 6 archetypes, Canny+Hough)
Single-Core
  throughput       49.7 Mpixel/s
  score             995   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      298.1 Mpixel/s
  score            5963

Photo Filter (10 photos, 1600x1200 px, 6 archetypes)
Single-Core
  throughput       28.4 Mpixel/s
  score             568   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      164.3 Mpixel/s
  score            3286

HDR (4 scenes x 6 exposures, 1600x1200 px, Mertens fusion)
Single-Core
  throughput        5.8 Mpixel/s
  score             964   (baseline 6.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       28.9 Mpixel/s
  score            4813

Ray Tracer (8 views, 480x360 px, 984 triangles, Whitted)
Single-Core
  throughput        5.2 Mray/s
  score            1038   (baseline 5.0 Mray/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       27.3 Mray/s
  score            5464

Structure from Motion (12 pairs, 640x480 px, 544 scene points, FAST+RANSAC)
Single-Core
  throughput       37.3 pairs/s
  score             373   (baseline 100.0 pairs/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      193.8 pairs/s
  score            1938

Composite Score (Integer 65% / Floating Point 35%)
  Single-Core
    score             596   (Integer 538, Floating Point 705)

  Multi-Core
    score            3272   (Integer 2962, Floating Point 3847)
```
