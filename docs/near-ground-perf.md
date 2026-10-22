# Near-ground performance

Status: investigated 2026-10-21. One fix landed (shared LOD tree); the
larger options below are on hold.

## How it was measured

- Scene: camera about 700 m above the terrain at the sub-solar point,
  2 km from a pivot on the ground, 20 degrees above its horizon, planet
  spin frozen. Bench scene file (not in `bench/scenes`, which is
  protected):

  ```
  view orbit
  pivot subsolar
  poff ground 0
  timescale 0
  frames 300
  simtime 0
  key 0   2000 20 0
  key 300 2000 20 40
  ```

  Run it with `cube.exe --bench <file> --out <json> [--width W --height H]`.
- New Bench.c options: `poff ground <m>` puts the pivot that far above
  the highest terrain within about 1 km (`Earth_GroundRadius` in Main.c);
  `timescale <x>` scales the sim clock (0 freezes the bodies). The JSON
  report now also has `walkMsAvg`, `nodesAvg` and `builtAvg` (quadtree
  walk CPU time per frame and nodes visited / computed).
- This laptop (Iris Xe) is thermally throttled: the same run can vary up
  to 3x. Compare builds only with alternating A/B runs, or use ratios
  within one run.
- WorldConfig.h is not a Makefile dependency. After changing it, touch a
  .c file so nmake rebuilds.

## Breakdown

| | 1280x720 | 2880x1800 (native) |
|---|---|---|
| Frame | ~25 ms | ~120 ms |
| Patches drawn (all walks) | 5.3k | 25k |
| Planet pass, GPU | ~12.5 ms | ~78 ms (~45 per-pixel, ~33 from triangle count) |
| Shadow pass, GPU | ~8 ms (cascades ~6) | ~26 ms |
| Quadtree walks, CPU | ~7 ms | ~40 ms |
| Atmosphere, GPU | ~2 ms | ~12 ms |
| Per-pixel detail octaves (3) | ~2.4 ms | ~13 ms (scaled estimate) |

Main points:

- **Triangle count drives most of it.** `QT_TARGET_TRIANGLE_PX` (24) is
  in physical pixels, so the patch count grows with screen height squared.
  The cascades redraw the casters with the main pass's LOD, and the walks
  visit nodes in proportion to patches, so both scale with it too.
  - Experiment: 60 px at 2880x1800 (the same patch count as 24 px at
    720p) cut patches from 25k to 5.7k, and shadow GPU from 26 to about
    10 ms.
- **Fragment cost** is the rest of the planet pass: quad.frag at native
  resolution with 2x MSAA, plus the per-sample depth prepass.
- **When the CPU is throttled**, the walks make the frame CPU-bound. A
  720p run measured 40 ms per frame with 25 ms of shadow-pass CPU.

## Done: shared LOD tree (QuadTree.c)

The split rule depends only on the camera. The main pass and the four
cascades therefore refine the same tree and differ only in culling and
maximum depth.
- The tree is now built lazily, once per frame per body and LOD target:
  each node's footprint, relief bounds, skirt, split decision and leaf
  instance are computed on first use and shared.
- Each walk only runs its own frustum and horizon tests.
- Results: pixel-identical images; walk CPU about 20% lower (about 8,000
  nodes visited, about 5,400 computed). The saving is small because the
  cascades cover different distance slices and overlap little with each
  other.
- The debug panel shows "nodes: N (M built)".

## On hold: options

1. **Resolution-independent LOD target** (recommended first). Interpret
   24 px at a 1080p reference and scale by window height, so it is about
   40 px on a 1800p screen.
   - Expect about a third of the patches, so less walk CPU, cascade GPU
     and planet vertex work.
   - Per-pixel detail normals keep the fine shading; silhouettes get
     coarser.
2. **75% render scale with sharpening:** see `render-scale.md`. It cuts
   per-pixel work (planet fragments, atmosphere, per-pixel octaves) and
   stacks with option 1.
3. **Coarser casters for the far cascades.** This cuts shadow vertex
   work, but the shadow mesh then no longer matches the visible mesh,
   which risks acne. Mitigate with bias or skip.
4. **Cross-frame caching in the walk.** Node footprints are body-local and
   fixed, and relief bounds only change when the chosen mip changes, so
   both could persist across frames. This needs a relief-callback contract
   that exposes the mip, or a cache keyed by (node, mip).
5. **Cheaper relief query** (`TerrainGen_HeightRange`): about a third of
   walk time. Its mip-0 exact path does up to 18x18 bilinear samples with
   integer modulo per sample.
6. **Staggered cascade updates:** re-render the far cascades every second
   frame.
