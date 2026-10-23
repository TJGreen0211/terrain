# HDR scene target (RGBA16F)

Status: **done** (2026-10-22, uncommitted). HDR is the only mode: the
legacy path was removed (Phase 6) without the matching pass (Phase 5), by
choice; the look is tuned forward from here, not back to legacy. Phase 0
was skipped too: the bench baselines and `visual_check` references still
show the legacy look and need re-baselining (the visual check fails until
then). Phase 1's perf cost was not measured.

## Goal

Render the main pass in linear radiance into an RGBA16F target and tone-map
once, at the resolve. This is the prerequisite for bloom/glare, exposure
control and a realistic sun (see the sun discussion; the sun work comes
after this).

Expect a lot of look tuning afterwards. The plan is built around that: the
legacy (current) look stays available at runtime as an A/B reference until
the new look is signed off, and every look parameter is a live slider.

## Where we are

- The scene target (`ensureSceneTarget`, `src/app/render.c`) is a 2x MSAA
  **RGBA8** renderbuffer + DEPTH32F multisample texture. It exists for
  reversed-Z depth; the colour format just matched the window.
- It holds **display values** (tone-mapped, gamma 2.2). Every lit shader
  calls `toneMap()` from `engine/gfx/shaders/tonemap.glsl` (ACES fit,
  `EXPOSURE` 1.05 as a `const`):
  terrain `quad.frag`, `water.frag`, `clouds.frag`, `ring.frag`,
  `rock.frag`, `planet_impostor.frag`, `gasgiant.glsl`, `nebula_vol.frag`,
  and the bake-only `nebula_post.frag` / `nebula_sky.frag`.
- Writers that do **not** go through `toneMap()`:
  - `skybox.frag`: writes the cube texture as is (display-encoded).
  - `atmosphere.frag`: adds `I` on top of display values, so the sky is
    never tone-mapped. Blend `(ONE_MINUS_DST_ALPHA, ONE)`.
  - `light.frag` (Moon, sun marker): plain `pow(1/2.2)`, no ACES.
  - `sun.frag`: clamps to [0,1]; glow is additive `(ONE_MINUS_DST_ALPHA, ONE)`.
  - `planets.c` dots: additive `(ONE, ONE)`.
- Display-space workarounds: `water.frag` reflects the sky through
  `displayToLinear(...)` (clamped at 0.95); `gasgiant.glsl` decodes its
  colour profile with `displayToLinear` (that one is a legit albedo decode).
- Resolve: `glBlitFramebuffer` MSAA -> window, then UI, then
  `Bench_MaybeCapture` reads the window as RGBA8 (unchanged by this plan).
- Scene alpha = cloud cover. Cleared to 0 after the planet; clouds write
  coverage; rings/nebula mask alpha off; the dome and sun glow scale by
  `1 - dst alpha`. RGBA16F keeps an alpha channel, so this keeps working.

## Key insight for tuning

For **opaque** pixels, "tone-map in the shader, store" and "store linear,
tone-map at resolve" give the same result, as long as the resolve uses the
same curve and the MSAA average happens after the tone map. So terrain, rocks,
impostors and gas giants should look **identical** after the switch. All
look changes come from:

1. **Blending in linear instead of display space**: water over seafloor,
   clouds, ring sheet, nebula, impostor edges/glow.
2. **Additive passes now going through the curve**: the atmosphere dome
   (the biggest change: ACES compresses the sky), the skybox, sun glow,
   planet dots.
3. **`light.frag`** moving from plain gamma to ACES (Moon, sun marker).

So the tuning surface is these passes, not the whole frame.

## Phases

Each phase ends green (build, `bench.ps1`, `visual_check.ps1`,
`check_modules.ps1`) unless stated otherwise.

### Phase 0: baselines and style shots

- Record perf on master with alternating A/B runs (laptop throttles).
- The four protected reference scenes don't cover the sun, rings, gas
  giants, nebula, clouds, night side or a sunset. Add a set of **style
  shots** outside the protected harness: camera scripts in `bench/style/`
  (not `bench/scenes/`, so `bench.ps1` doesn't pick them up), captured with
  `cube.exe --bench <file> --shots ...` into `bench/style/legacy/`. These
  legacy captures are the look target for Phase 5. You pick the views that
  matter most for the style.

### Phase 1: RGBA16F target + resolve pass (no look change) - DONE

Done as described below: `engine/gfx/scene_target.{c,h}`,
`engine/gfx/shaders/resolve.{vert,frag}` (per-sample clamp to [0,1], then
average, alpha included because bench shots save RGBA), `PROF_RESOLVE`
("Resolve"; the UI section is now just "UI"). Explicit [0,1] clamps were
added where RGBA8 used to clamp: `planet_impostor.frag` (disc and dots),
`clouds.frag`, `light.frag`, `ringdust.frag`, and the sun glow (its f term
goes negative far out, which a float target would subtract). The dome is
left unclamped: it and the sun glow after it only add, so the per-sample
clamp in the resolve gives the same result. `visual_check.ps1`: all frames
match (SSIM >= 0.9987).


- Colour attachment becomes an `RGBA16F` `GL_TEXTURE_2D_MULTISAMPLE`
  (fixed sample locations, like the depth), so a shader can `texelFetch`
  the samples.
- Move the scene target out of `render.c` into an engine module
  `engine/gfx/scene_target.{c,h}` (`SceneTarget_`), with a sample count
  and size passed in, since `engine/` can't include `WorldConfig.h`.
  `render.c` keeps the call order.
- Replace the blit with a fullscreen resolve shader
  `engine/gfx/shaders/resolve.frag`. In this phase it averages the samples
  and clamps to [0,1], which is what the blit did. Viewport and draw go
  straight to the window. Add `PROF_RESOLVE` to the profiler so its cost
  shows up separately from the UI.
- Shaders unchanged (still display values).
- **Gotcha: unclamped outputs.** RGBA8 clamps every fragment output and
  blend result to [0,1]; RGBA16F doesn't. Anything that relied on that
  clamp (the dome adding past 1.0 before the sun glow adds again, negative
  values, alpha > 1 feeding `ONE_MINUS_DST_ALPHA`) can now differ. Fix by
  clamping at the source (`clamp(alpha, 0, 1)`, `max(rgb, 0)`) rather than
  in the resolve.
- **Gate:** `visual_check.ps1` passes against the existing references.
  Perf delta of the format change measured alone (expected cost: twice the
  colour bandwidth, ~43 -> ~87 MB at 2880x1880x2). If it's too expensive,
  stop here and decide; the fallback (R11F_G11F_B10F) needs the cloud-cover
  alpha redesigned first.

### Phase 2: shared tone-map parameters + mode switch - DONE

Done as described below, with these differences:
- The block is `ToneMapBlock { vec4 toneMapSettings; }` (x exposure,
  y HDR mode); only exposure and the mode so far. Curve choice and grading
  come in Phase 4.
- `displayToLinear` stays the true inverse of the curve in both modes:
  the gas giants use it to decode albedo, which must not change. Water's
  sky reflection is the one call site to switch in Phase 3.
- The nebula bakes (`nebula_sky.frag`, `nebula_post.frag`) define
  `TONEMAP_DISPLAY` before the include: always the curve at exposure
  1.05, so a re-bake gives the same sky whatever the runtime settings.
- The resolve already tone-maps each sample (ACES) in HDR mode, so the
  switch is usable now; Phase 4 adds the alternative curves, grading and dither.
- F2 toggles the mode; `--hdr` starts in HDR mode (for HDR bench shots).
  `ToneMap_Init` runs before `Water_Init`, since init-time bakes read the block.
- Legacy mode still matches the references (same SSIM as Phase 1). HDR
  mode renders cleanly but washed out: the dome and the skybox still write
  display values, which the resolve now curves as if they were linear.
  That is Phase 3.


- New engine module `engine/gfx/tone_map.{c,h}` (`ToneMap_`): owns a
  **uniform buffer** (GL 3.1+, fine on 4.1) with exposure, mode, curve
  choice and grading values. `ToneMap_Params()`, `ToneMap_DebugPanel`
  registered in `app.c` ("Tone map" tab), uploaded once per frame.
- `Shader_Create*` binds the block to its binding point when a program
  declares it (`glGetUniformBlockIndex` + `glUniformBlockBinding`; GLSL
  4.10 has no `layout(binding=)`), so modules don't each do it.
- `tonemap.glsl` declares the block. `toneMap(c)` becomes:
  - **legacy mode**: the current ACES + gamma (display out), as today;
  - **HDR mode**: `max(c, 0) * preExposure` (linear out).

  `displayToLinear` likewise returns its input unchanged in HDR mode where
  the input is already linear (water's sky; see Phase 3).
- The resolve shader reads the mode: legacy = average + clamp (Phase 1),
  HDR = tone-map (Phase 4).
- Result: **both looks live in one build, switchable at runtime** (debug
  tab checkbox + a key). This is the main tuning tool. The default stays
  legacy until Phase 5 sign-off, so the harness stays green.

### Phase 3: convert the non-toneMap writers (HDR mode only) - DONE

Done, legacy untouched (references still match). How it differs from the
sketch below:
- One rule for every pass still authored as display values:
  `sceneFromDisplay(d)` in `tonemap.glsl` returns `d` in legacy mode and,
  in HDR mode, the exact linear pre-image of `d` under the curve at the
  default exposure (1.05). Over black these passes look exactly as before,
  and the exposure slider scales them like everything else. Used by the
  skybox, the dome (`I`), the air `clouds.frag` adds in front of a cloud,
  the sun disc and glow, the rocky stand-ins (`lit`, limb glow, dots) and
  ring dust (gamma-encoded in `ringdust.vert`).
- Additive passes over a lit surface do not match exactly: the dome's
  haze now adds in linear before the curve, so it is weaker over bright
  ground and sea (grazing-sea, topdown-detail). That is the first thing
  to tune.
- HDR gains, all on the **Tone map** tab (`app.c` `toneMapPanel`, next to
  exposure): Sky (`Skybox_HdrGain`), Atmosphere (`Atmosphere_HdrGain`, dome
  and clouds' air; one value for every planet, since `atmosphereParams`
  is swapped per planet), Sun (`Sun_HdrGain`), Stand-ins
  (`Planets_HdrGain`). Each is a `float *` accessor.
- `light.frag` (Moon, sun marker): plain gamma in legacy as before; in HDR
  linear, so ACES at the resolve (Moon shifts slightly).
- The Phase 1 clamps became `sceneOut()`: [0,1] in legacy, `max(0)` in
  HDR, so HDR values above 1 survive. Alpha is still clamped.
- Water needed no change: its reflection already decodes the display sky
  with `displayToLinear` and goes through `toneMap()`. (The Sky gain does
  not reach the reflection.)
- The NaN/Inf debug view moves to Phase 4, with the rest of the resolve
  work.


Each one branches on the mode so legacy stays exactly as it is:

- **Skybox**: decode with the exact inverse of the legacy curve
  (`displayToLinear` without the 0.95 clamp, or a widened one), so an
  opaque sky round-trips to the same pixels. Plus a `skyGain` slider.
  (Later: keep the nebula bake's RGBA16F cube instead of the 8-bit faces
  for real star brightness.)
- **Atmosphere dome**: output `I` as linear radiance times a new
  `Atmosphere_Params()->hdrGain`. There's no exact match for "added after
  the curve"; tune in Phase 5.
- **Water reflection**: the sky colour it builds (`skyDisp`) is linear in
  HDR mode; skip `displayToLinear` there.
- **`light.frag`**: drop the manual gamma, call `toneMap()`. The Moon will
  shift slightly (plain gamma -> ACES) even in legacy mode unless gated; gate it.
- **Sun** (`sun.frag`): output linear with a temporary `sunGain`; no clamp.
  It gets replaced in the sun work anyway.
- **Planet dots** (`planets.c` additive): through `toneMap()` + gain.
- **fp16 safety**: `max(0)` at every output; the resolve shader gets a
  debug view that paints NaN/Inf magenta and >65000 cyan. Expect to find
  existing NaNs that RGBA8 hid; fix at the source. Keep the sun disc within
  ~1e3–1e4 of mid-grey (pre-exposed units), well under fp16 max.

### Phase 4: resolve tone mapping - DONE

Done in `resolve.frag` / `tone_map.c`. Legacy output is untouched
(references match); HDR with defaults equals Phase 3 plus dither (max 1 LSB).
- Per sample in HDR mode: white balance (linear, luminance-preserving
  scales from temperature/tint) -> exposure -> curve -> grading, then
  average, then triangular +-1 LSB dither (fixed per pixel, toggleable).
- Curves (`ToneMapCurve`): ACES fit (default, the legacy curve), Khronos
  PBR Neutral, AgX (Wrensch's minimal fit), extended Reinhard (white point
  slider). The ACES fit stays the curve of `toneMap()` in legacy mode and
  of `displayToLinear` / `sceneFromDisplay`, so with another curve the
  display-authored passes no longer match exactly over black; tune their
  gains. AgX and Reinhard read darker and flatter at the same exposure.
- Grading after the curve (display space): saturation, contrast (around
  0.5), lift / gamma / gain per channel. "Reset curve + grading" button.
- Debug view (both modes): invalid values (NaN/Inf magenta, > 59000
  cyan, negative yellow) and luminance in EV around 0.18 (black < -6, blue,
  cyan, green ~0, yellow, red, white > +6). Nothing invalid showed up in
  the four bench scenes' grazing-sea shot.
- The block is now six vec4s (`toneMapSettings`, `toneMapGrade`,
  `toneMapBalance`, `toneMapLift`, `toneMapGamma`, `toneMapGain`).
- Command line: `--curve <n>` and `--tonemap-debug <n>` (enum values),
  with `--hdr`, for captures.


- HDR-mode resolve: **tone-map each sample, then average** (what the RGBA8
  path did implicitly). Averaging linear samples first loses anti-aliasing
  on bright edges (sun against a limb, glints).
- Curve: start with the legacy ACES fit, so opaque pixels match. Other
  curves selectable in the tab: Khronos PBR Neutral, AgX, Reinhard-extended.
  Exposure goes from `const` to the uniform.
- Display grading after the curve, all neutral by default: saturation,
  contrast, lift/gamma/gain, white balance. This is where the stylised
  look gets recovered if the linear blends drift.
- Triangular dither on the 8-bit output (reuse the nebula bake's).

### Phase 5: tuning (the long one) - SKIPPED

Not done as a match to legacy: the user chose to commit to HDR and tune the
look forward instead. The tools stay (Tone map tab: exposure, curve,
grading, per-pass gains; debug views). The original procedure, for
reference:

Procedure:

1. Toggle legacy/HDR on each style view; tune the HDR-only knobs (dome
   gain, sky gain, sun/dot gain, curve, grading) until it matches or
   deliberately improves on legacy.
2. Capture the style shots in HDR mode into `bench/style/hdr/` and compare
   with `bench/style/legacy/` side by side. Running `visual_check.ps1`
   against the existing references in HDR mode gives a rough "distance
   from the old look" number (read-only use; don't update refs yet).
3. Per-module order, biggest first: atmosphere dome -> water -> clouds ->
   rings -> nebula -> skybox -> impostors/dots -> Moon.
4. Move tuned defaults into the module params (and `WorldConfig.h` where
   they're world constants).

Sign-off (you):

- Switch the default to HDR.
- **Re-baseline** `visual_check` references (`-UpdateRefs`) and perf
  (`-UpdateBaseline`), then `-SealManifest`. Human-reviewed only.

### Phase 6: cleanup - DONE

HDR is the only mode. Removed: the `hdr` param, F2, `--hdr`, `sceneIsHDR`
and `sceneOut`, the legacy branch in every shader and in the resolve
(`toneMapSettings.y` is now unused). `toneMap()` returns linear radiance
(clamped at 0) except under `TONEMAP_DISPLAY` (nebula bakes), and
`sceneFromDisplay` always converts. Comments updated. Output is identical
to Phase 4 HDR (max 1 LSB, the dither). Still open: water's
`displayToLinear` keeps its 0.95 clamp (caps how bright a reflected sky
can get); the per-pass gains are named `*_HdrGain`.

Original list:

- Remove legacy mode: the branches in `toneMap`/`displayToLinear`, the
  per-shader legacy paths, the 0.95 clamp workaround in water.
- Update comments that say the target holds display values
  (`tonemap.glsl`, `render.c`, `docs/volumetric-clouds.md`).

## After this (out of scope)

- Bloom/glare (dual-filter pyramid off the HDR target, before the curve).
- Auto-exposure / eye adaptation (luminance histogram or log-average).
- Sun rewrite: 3D photosphere, glare scaled by visible fraction, corona,
  coronal loops.
- Render scale: shares this resolve pass (`docs/render-scale.md` step 3).

## Risks

- **Perf on Iris Xe**: blended full-screen passes are bandwidth-bound.
  Measured at the end of Phase 1, before any look work.
- **Look drift in blended passes**: mitigated by the live A/B switch,
  gain sliders and post-curve grading; some drift may be preferable
  (linear blending is more correct).
- **Hidden NaN/negative/out-of-range outputs**: surfaced by the Phase 3
  debug view.
- **Legacy-mode branches cost a little** in every shader until Phase 6.
  It's a uniform branch, so cheap, but remove it promptly.
