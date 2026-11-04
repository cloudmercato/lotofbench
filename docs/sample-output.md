# Sample output

A captured default run of LotofBench 1.0.0 on the development box
(12 online threads, `./lotofbench --json-out docs/sample-output.json`,
2026-11-04, background load ~5 of 12 threads — the box's own services).
The text report is the tool's stdout, verbatim; the same run's
machine-readable results are kept in
[sample-output.json](sample-output.json). Regenerate both files with
the same command after a deliberate output change.

```
LotofBench 1.0.0
seed 1, 5 runs, median

File Compression (corpus 100 MB synthetic)
Single-Core
  throughput      107.3 MB/s
  score             429   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      593.2 MB/s
  score            2373

Navigation (grid 512x512, 64 routes)
Single-Core
  throughput       43.1 Medge/s
  score             431   (baseline 100.0 Medge/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      228.6 Medge/s
  score            2286

HTML5 Browser (32 pages, 800x600 viewport, 3544 DOM elements)
Single-Core
  throughput      297.7 pages/s
  score             458   (baseline 650.0 pages/s = 1000)

Multi-Core (12 threads, shared task)
  throughput     1787.4 pages/s
  score            2750

PDF Render (4 park maps, 2 pages each, 1224x1584 px @2x)
Single-Core
  throughput        3.1 Mpixel/s
  score             313   (baseline 10.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       14.0 Mpixel/s
  score            1399

Photo Library (16 photos, 1280x960 px, 8 classes)
Single-Core
  throughput      196.2 img/s
  score             981   (baseline 200.0 img/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      876.9 img/s
  score            4384

Clang (32 units, 986328 source tokens)
Single-Core
  throughput     4843.8 Ktokens/s
  score             570   (baseline 8500.0 Ktokens/s = 1000)

Multi-Core (12 threads, shared task)
  throughput    23798.5 Ktokens/s
  score            2800

Text Processing (32 docs, 8 patterns/doc)
Single-Core
  throughput       96.7 MB/s
  score             484   (baseline 200.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      435.6 MB/s
  score            2178

Asset Compression (16 assets (12 textures, 4 meshes))
Single-Core
  throughput       45.8 MB/s
  score             916   (baseline 50.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      207.5 MB/s
  score            4149

Object Detection (8 photos, 300x300 px, int8 convnet, SSD-lite head)
Single-Core
  throughput       20.3 img/s
  score             811   (baseline 25.0 img/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       98.5 img/s
  score            3939

Background Blur (12 frames, 1920x1080 px, mask + blur composite)
Single-Core
  throughput       15.6 fps
  score             313   (baseline 50.0 fps = 1000)

Multi-Core (12 threads, shared task)
  throughput       90.1 fps
  score            1802

Object Remover (4 photos, 1600x1200 px, push-pull inpainting)
Single-Core
  throughput       33.9 Mpixel/s
  score            1131   (baseline 30.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      162.6 Mpixel/s
  score            5419

Horizon Detection (8 photos, 1600x1200 px, 6 archetypes, Canny+Hough)
Single-Core
  throughput       48.9 Mpixel/s
  score             979   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      272.5 Mpixel/s
  score            5450

Photo Filter (10 photos, 1600x1200 px, 6 archetypes)
Single-Core
  throughput       28.3 Mpixel/s
  score             565   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      154.4 Mpixel/s
  score            3088

HDR (4 scenes x 6 exposures, 1600x1200 px, Mertens fusion)
Single-Core
  throughput        5.9 Mpixel/s
  score             978   (baseline 6.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       26.4 Mpixel/s
  score            4403

Ray Tracer (8 views, 480x360 px, 984 triangles, Whitted)
Single-Core
  throughput        5.2 Mray/s
  score            1039   (baseline 5.0 Mray/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       27.3 Mray/s
  score            5466

Structure from Motion (12 pairs, 640x480 px, 544 scene points, FAST+RANSAC)
Single-Core
  throughput       37.2 pairs/s
  score             372   (baseline 100.0 pairs/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      182.1 pairs/s
  score            1821

Composite Score (Integer 65% / Floating Point 35%)
  Single-Core
    score             594   (Integer 534, Floating Point 704)

  Multi-Core
    score            2973   (Integer 2634, Floating Point 3603)
```
