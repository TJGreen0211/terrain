# Render scale (draw the 3D scene below window resolution)

Status: **not implemented** (planned optimization). Written 2026-10-20.

## Idea

Render the 3D scene into the off-screen scene target at a fraction of the
window size (e.g. 0.75 of width and height), then upscale it to the window.
The UI and debug panel still draw at full resolution, so text stays sharp.

0.75² ≈ 56% of the pixels, so passes limited by per-pixel work shrink by
roughly 40%. Unaffected: shadow map, FFT wave simulation, quadtree walks.

## Why it is the remaining big lever

Measured on the dev machine (Intel Iris Xe, 2880×1880, 2× MSAA) after the
LOD and shader optimizations. Per-pass numbers are in the debug panel
(F1 → Performance, CPU / GPU ms per pass).

| View | fps | Planet GPU | Water GPU |
|---|---|---|---|
| Orbit | 27 | 8.7 ms | 5.7 ms |
| Ocean from 637 km (surface preset, G) | 23 | 7.1 ms | 14.8 ms |
| Land from 637 km | 26 | 22.6 ms | 0 |

Everything big is full-screen fragment work. The water shader has no
single hot spot: each feature costs 0.5–1.6 ms, and with every optional
feature disabled it still costs ~8 ms. That is the price of its texture
lookups and blending at this resolution, and only fewer pixels reduce it.

Rough estimate (not measured): ocean view 23 → ~30 fps, orbit 27 → ~33 fps.

## What it involves

The scene already renders into its own multisampled target
(`ensureSceneTarget` in `src/Main.c`, resolved into the window with
`glBlitFramebuffer` before the UI), so the change is contained.

1. **Config.** Add `RENDER_SCALE 0.75` to the rendering section of
   `src/WorldConfig.h` (next to `RENDER_MSAA_SAMPLES`). Optionally add a
   debug-panel slider to change it live; the target already re-creates
   itself when its size changes.
2. **Smaller target.** In the main loop, compute
   `sceneW = windowW * RENDER_SCALE`, `sceneH = windowH * RENDER_SCALE`,
   pass those to `ensureSceneTarget`, and set the main pass viewport to
   them.
3. **Two-step resolve.** `glBlitFramebuffer` cannot change size while
   resolving MSAA, so:
   - resolve the MSAA target into a single-sample colour target of the
     same (scaled) size;
   - stretch that to the window: `glBlitFramebuffer(..., GL_LINEAR)`, or
     a small full-screen shader that also sharpens (see below).
4. **Pixel-count inputs must use the render size, not the window.** These
   currently call `getWindowHeight()` meaning "pixels on screen":
   - `setBodyUniforms` (`lodK`, the height-map mip for the pixel footprint)
   - `earthReliefBounds` and `footprintAllLand` (same `lodK` formula; must
     match the shaders or culling bounds drift)
   - `buildWalk` and `makeKey` in `src/geometry/QuadTree.c` (split factor
     K, so `QT_TARGET_TRIANGLE_PX` stays in rendered pixels)
   - `Rings_DrawDust` in `src/Rings.c` (`projScale`, dust point size)

   Simplest: add a `getRenderHeight()` (scene target height) and use it in
   all of these. The sun billboard reads `GL_VIEWPORT` and adjusts by
   itself. Shaders that use `dFdx`/`dFdy` mip selection adjust by
   themselves.
5. **UI** keeps drawing to the default framebuffer at window size after
   the stretch (unchanged).

Cost: the extra stretch is ~0.5 ms at this resolution.

## Sharpening (recommended with the upscale)

A single-pass contrast-adaptive sharpen (the idea behind AMD CAS) in the
stretch shader recovers most of the lost crispness: sample the 3×3
neighbourhood, compute local min/max, and apply a sharpening weight that
backs off where contrast is already high. About 20 lines of GLSL; cost
well under 1 ms.

## Risks

- Missing one of the pixel-count inputs in step 4: LOD density, mip
  selection or ring dust size would look off by the scale factor.
- `footprintAllLand` and water.frag's discard must keep using the same
  `lodK`, or the water culling could drop visible water near coasts.
- Picking or anything else that maps mouse coordinates into the scene
  (none today) would need to scale by `RENDER_SCALE`.

## Follow-up: dynamic resolution

Once the scaled target and stretch exist, adjust the scale each frame to
hold a target frame time (e.g. step it between 0.6 and 1.0 based on the
last few frames' GPU time from the profiler). The target re-creation
should then be avoided: allocate at full size once and render into a
sub-rectangle, stretching from that rectangle.

## Verification

- Debug panel Performance section: planet, water and atmosphere GPU ms
  should drop ~40% at 0.75; shadow map and waves unchanged.
- Debug panel Camera section: patch count drops slightly (LOD now in
  rendered pixels), which is expected.
- Compare frames at 1.0 and 0.75 at the same frozen view for softness and
  for any LOD or dust-size mismatch.
