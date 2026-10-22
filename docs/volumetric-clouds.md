# Volumetric clouds

Status: **step 1 (2D slab) paused, switched off by default** (2026-10-21).
`CLOUDS_ENABLED_DEFAULT` is 0; the debug panel's Clouds section has the
switch and every setting. With clouds off the app renders pixel-identical
to before the cloud work. See "Step 1 as built" at the end for where it
stands and what is open. Design written 2026-10-21, revised the same day
after the procedural solar system landed (see `notes/solar-system-5.md`).

Goal: cloud layers on every planet with an atmosphere (Earth and the
generated rocky and dwarf planets) that look right from the surface up to
orbit, with one set of quality knobs that can be changed at runtime so
the same code runs on the dev machine (Intel Iris Xe, 2880×1880) and on
faster GPUs. Nothing is Earth-specific: every scale is per planet.

The approach follows the "raymarched cloud shell at reduced resolution,
accumulated over frames" line of work from Horizon Zero Dawn [1, 2] and
Frostbite [3, 4], adapted to this project's planet scale, floating origin
and OpenGL 4.1 target. Numbers in brackets refer to the references at the
end.

## Constraints that shape the design

- **OpenGL 4.1 core** (`src/Window.c`, kept for macOS). No compute
  shaders, no image load/store, no SSBOs. Every pass, including noise
  generation, is a fragment shader writing to an FBO; 3D textures are
  filled one slice at a time with `glFramebufferTextureLayer`.
- **Planets are full size.** A cumulus layer of ~1.5–8 km is ~0.1% of
  Earth's radius. From the ground a horizontal ray crosses hundreds of km
  of layer; from orbit one pixel covers several km. No single march
  setting serves both, so detail must follow the pixel footprint, the
  same idea as the terrain's `lodK` (`setBodyUniforms` in `src/Main.c`).
- **The full-detail planet starts at 3 px radius** (`Planets_FullDetail`).
  The camera can then be ~10⁴ radii away, where the textbook ray-sphere
  quadratic cancels in float. Cloud rays use the stand-ins' stable form
  (body-radius units, miss distance from |o×d|).
- **The GPU budget is already spent.** `notes/render-scale.md` measures
  23–27 fps in the main views. Full-resolution volumetrics are not
  affordable on the dev machine; low resolution plus temporal
  accumulation is required, not an optional upgrade.
- **Floating origin.** Shaders only see camera-relative or body-local
  floats. Sample noise in planet-local coordinates (float precision at
  6.4e6 m is ~0.5 m, ample for km-scale noise). Anything that grows with
  time (flow phases) is computed in double on the CPU and wrapped.
- **One active planet.** Only one terrain planet is drawn in full; its
  state lives in globals that `saveActive` / `loadActive` swap
  (`src/SolarSystem.c`). Everything else is a stand-in
  (`src/Planets.c`, `shaders/planet_impostor.frag`).
- **The scene is LDR.** The main target is RGBA8 holding tone-mapped,
  gamma-encoded colour: every lit shader applies the ACES curve itself,
  the atmosphere dome then adds on top. Clouds tone-map their own output
  and blend in display space, like the water.
- **Existing atmosphere.** The O'Neill single-scattering dome [5]
  (`src/Atmosphere.c`, `src/shaders/atmosphere.frag`) is additive and ends
  its rays at scene depth. Clouds have to occlude it and be fogged by it.

## Per-planet cloud settings

`cloudParams` follows `atmosphereParams` exactly: a global `g_clouds` for
the active planet (edited live from the debug panel's Clouds section),
saved into and loaded from `PlanetInfo.clouds` by `saveActive` /
`loadActive`. Earth's values are in `src/WorldConfig.h`; other planets'
are generated in `SolarSystem_Generate` from their own RNG stream, so the
existing generated planets do not change.

| Field | Meaning |
|---|---|
| style | none, climate (moisture-driven), patchy, banded, overcast |
| coverage | target sky fraction |
| climateWeight | climate style: how strongly moisture and latitude belts steer coverage |
| altitude, thickness | layer middle and depth above sea level (m) |
| opacity | optical depth of a full column |
| weatherScale | largest weather system, as a fraction of the planet radius |
| sharpness, detail | edge hardness; erosion by the small-scale noise |
| bands | banded style: bands pole to pole |
| color | cloud albedo |
| flowPeriod | seconds of simulation time per flow cycle (0 = frozen) |

Who gets which style:

- **Earth:** climate style, driven by the baked moisture map.
- **Rocky and dwarf planets with an atmosphere:** a random style, from
  none through patchy and banded to Venus-like overcast. Planets that
  generated `hasWater` get the climate style (their moisture map then
  stands in for weather). Layer altitude is a fraction of the planet's
  atmosphere shell, so a layer never pokes out of a small planet's air.
- **Planets without air:** none.
- **Gas giants:** untouched. They are stand-ins only, and their bands are
  their clouds.

## Weather map (per planet)

The terrain bake already produces, per planet, a moisture map
(`climateMap`, R) and a prevailing-wind map (`oceanTex`: speed, east and
north components) in the height field's equirect layout. The weather map
is built from them on the GPU (`shaders/cloudmap.frag`):

- **R = coverage** at the map's scale: the style's coverage potential
  (moisture × latitude belts for the climate style: wet ITCZ, dry
  subtropics, mid-latitude storm tracks) thresholded against
  domain-warped FBM noise [1, 2] (value noise on an integer hash,
  evaluated in 3D so there are no seams or pole pinching).
- **G = column thickness**, thicker where the potential is high (storms).
- 1024×512 RGBA8 with mips, same equirect convention as the climate map
  (`mapUV` in `quad.frag`). About 39 km per texel on Earth.

**Motion.** The noise domain is advected along the wind map with the
two-phase flow-map technique [16]: two copies of the noise displaced by
wind × τ, τ cycling over `flowPeriod`, half a cycle apart, blended with
triangle weights (and contrast-corrected, so coverage does not pulse).
Each cycle re-seeds its copy, so the pattern never repeats. This avoids
the unbounded shearing that integrating a latitude-varying wind would
cause. The CPU computes the phases in double and wraps them.

**Rebuilds.** Each planet keeps two maps, for simulation times
k·Δ and (k+1)·Δ (Δ = `CLOUD_MAP_STEP_S`, one sim hour), crossfaded by the
time in between. One new map is built when the time crosses a step; at
most a couple of builds per frame (fast time scales just rebuild every
frame). A planet's maps are dropped with its bake and rebuilt when its
cloud settings change.

**Detail below the map.** The slab adds noise octaves from ~4 map texels
down to two pixels of footprint, advected with the same two flow phases,
eroding thin coverage and leaving thick cores intact [1]. Octaves fade to
their mean as they shrink below a pixel, so coverage does not change with
distance.

## Step 1: the 2D slab (`src/Clouds.c`, `shaders/clouds.frag`)

The layer is shaded as a single surface at its middle altitude, for the
active planet.

- **Geometry.** A camera-facing quad around the cloud sphere's silhouette
  (the stand-ins' approach), or a full-screen quad when the camera is
  inside or near the layer. Stable ray-sphere in body radii; the first
  hit in front of the camera (outer side from above, inner side from
  below).
- **Opacity.** Column optical depth τ = opacity × coverage × thickness
  from the weather map and detail. The view path through the slab is
  τ/μ, with μ floored by the longest chord through a shell of that
  thickness (grazing rays). α = 1 − e^(−τ/μ).
- **Light.** A plane-parallel cloud's diffuse reflectance and
  transmission, R = 1 − T_d, T_d = 1 / (1 + 0.75 (1 − g) τ) [3]: the
  tops reflect R × the sun's Lambert term, the base (seen from below)
  lets through T_d. Sunlight is reddened and cut off at the terminator
  by the shared `sunTransmittance`, shadowed by the planet's rings, and
  the same skylight term the ground uses lights the shadowed side.
  Extinction between camera and cloud uses the shared `applyAtmosphere`,
  then the shared tone map.
- **Depth and order.** Drawn after the water and before the rings and the
  dome, depth-tested against the scene. A second, depth-only draw writes
  the cloud depth where α > 0.5, so ring sheets behind thick clouds are
  hidden and the dome's in-scatter ends at the cloud top with no dome
  changes. Thin clouds still let a little sky glow through; step 4 fixes
  that properly.
- **Cloud shadows.** `quad.frag` and `water.frag` follow the sun ray to
  the layer and read the weather maps there: the direct sun is dimmed by
  e^(−τ/μ_s), with the diffusely transmitted part (T_d) let back in.
  Map only, no detail octaves: two texture reads per pixel.
- **Stand-ins.** `planet_impostor.frag` reads a baked planet's weather
  maps and lays the coverage over the disc, so the switch to full detail
  does not add a cloud pop.

Preparation (no visual change): the atmosphere extinction, sunlight,
ring shadow and tone map code duplicated across `quad.frag`,
`water.frag`, `ring.frag` and `rock.frag` moves into `atmosurface.glsl`,
`ringshadow.glsl` and `tonemap.glsl`, so the clouds use exactly the same
functions.

## Later steps: the volume

### 2. Raymarch pass

Full-screen pass into an RGBA16F target at `resScale` × scene size.

- **Shape noise.** 128³ RGBA8 Perlin-Worley [6, 7] for the base shape and
  32³ Worley FBM for erosion, distorted by curl noise [8]; baked once
  behind the loading screen, one draw per slice. The weather map's
  coverage and a per-style height profile [1, 2] shape the density.
- **Ray setup.** Intersect the layer's inner and outer spheres (camera
  below, inside, or above), clip at scene depth as `sceneDistance` in
  `atmosphere.frag` does.
- **Stepping.** Step size grows with distance; ray start jittered with
  blue noise [9]; cheap steps through empty space, full detail once
  density shows up [1].
- **Lighting per sample.** Light steps toward the sun, Beer–Lambert with
  Hillaire's energy-conserving integration [3] (stable when step count
  changes, which matters because it is a quality knob), the powder term
  [1], a two-lobe Henyey–Greenstein phase [10], and the multiple
  scattering octaves of Wrenninge et al. [11]. Sun colour from the shared
  `sunTransmittance`; ambient from the sky.
- **Output.** In-scattered light, transmittance, and a
  transmittance-weighted cloud distance.

### 3. Temporal accumulation and upsampling

- March one pixel of every N×N block per frame and reproject the rest
  [1, 2], with neighbourhood clamping against stale history [12].
- Reprojection matrices built on the CPU in double, since the
  camera-relative frame moves its origin every frame.
- Joint-bilateral upsample [13] against the full-resolution scene depth.

### 4. Interaction with the atmosphere

- The dome's rays end at `min(sceneDist, cloudDist)`, blended by cloud
  transmittance, replacing step 1's depth-write approximation.
- Longer term, a LUT sky [14] gives cloud ambient and aerial perspective
  from the same tables.

### 5. Distance-based detail

The step-1 slab stays as the far field and the orbit view: the volume
fades into it across a distance band, as in Red Dead Redemption 2 [15].
Erosion octaves fade out by pixel footprint (`lodK`'s formula).

### 6. Better shadows and reflections

- A cloud transmittance map from the sun around the camera (like the
  first cascade in `src/Shadows.c`), updated every few frames.
- Ocean reflections of the slab along the reflected ray in `water.frag`.

## Quality: one struct, changeable at runtime

Every cost driver lives in one struct. Defaults and presets go in
`src/WorldConfig.h`; the live copy is edited from the debug panel.

```c
typedef struct cloudQuality {
	float resScale;         // march resolution / scene resolution, 0.25 .. 1.0
	int   temporalBlock;    // 1, 2, 4: one pixel marched per NxN block per frame
	int   viewSteps;        // max primary steps, 32 .. 128
	int   lightSteps;       // steps toward the sun, 2 .. 6
	int   msOctaves;        // multi-scatter octaves [11], 1 .. 3
	int   detailNoise;      // erosion noise on/off
	float volumetricRangeM; // beyond this distance: 2D slab only
	int   slabOctaves;      // slab detail octaves (step 1)
	int   shadowMode;       // 0 none, 1 weather map, 2 shadow map
} cloudQuality;
```

| Preset | resScale | block | view / light steps | MS octaves | Detail | Shadows |
|---|---|---|---|---|---|---|
| Low (Iris Xe) | 0.25 | 4×4 | 32 / 2 | 1 | off | weather map |
| Medium | 0.5 | 2×2 | 64 / 4 | 2 | on | weather map |
| High | 0.5 | 1×1 | 96 / 5 | 3 | on | shadow map |
| Ultra | 1.0 | 1×1 | 128 / 6 | 3 | on | shadow map |

These numbers are starting guesses, to be tuned with the profiler.

- **Live, as uniforms:** step counts, octaves, ranges.
- **Re-created target:** `resScale` and `temporalBlock` (history reset).
- **Recompile:** structural switches (`detailNoise`, `shadowMode`) as
  `#define`s through `LoadShaderDefines`, recompiled on change.
- **Automatic mode (optional):** steer `resScale` and `viewSteps` toward
  a millisecond budget from `PROF_CLOUDS`, with hysteresis.

## Files

| File | Role |
|---|---|
| `src/Clouds.c/.h` | Params, weather maps, slab draw, uniforms for receivers and stand-ins |
| `src/shaders/cloudnoise.glsl` | Hash, value noise, FBM, flow phases |
| `src/shaders/cloudmap.frag` | Weather map build |
| `src/shaders/cloudlayer.glsl` | Weather lookup, detail, shadow transmittance |
| `src/shaders/clouds.vert/.frag` | The slab |
| `src/shaders/atmosurface.glsl`, `ringshadow.glsl`, `tonemap.glsl` | Shared surface lighting |
| `src/SolarSystem.c/.h` | `PlanetInfo.clouds`, generation, save/load |
| `src/WorldConfig.h` | Earth's clouds, generation ranges, map size and step |
| `src/Main.c`, `src/Planets.c` | Pass order, receivers, stand-ins |
| `src/DebugGUI.c`, `src/Profiler.h` | Clouds panel, `PROF_CLOUDS` |

## Risks

- **Reprojection with the floating origin** (step 3): ghosting when
  moving fast at low altitude.
- **Inside the layer** the ray starts in cloud; near steps must be small.
- **Horizon from the ground**: rays crossing hundreds of km of layer will
  run out of steps; the slab far field must cover it.
- **Flow-map pulsing**: the two-phase blend can show a faint rhythm at
  half the flow period; the contrast correction and re-seeding hide most
  of it.

## Verification

- Debug panel Performance: `PROF_CLOUDS` GPU ms in orbit, at the
  637 km views and at the surface.
- Visual: the refactor must be pixel-identical; then sunset tint, night
  side dark, ring shadows on the clouds, cloud shadows on land and sea,
  stand-in to full-detail switch, and each style on a generated planet.
- The bench references are left as they are for now (the user will
  update them); clouds change every Earth scene.

## Step 1 as built (2026-10-21, paused)

Done and verified by rendering (bench scenes and test scenes in the
session scratchpad, not checked in):

- **Shared shader code** (no visual change, verified pixel-identical):
  `atmosurface.glsl`, `ringshadow.glsl`, `tonemap.glsl`, and the dome's
  march in `atmoscatter.glsl`. Shader `#include`s now nest
  (`LoadShader.c`, depth limit 8).
- **Per-planet settings**: `cloudParams` in `PlanetInfo.clouds` and
  `g_clouds`, saved and loaded with the other planet globals. Generated
  from their own RNG stream (`generateClouds`), so existing seeds keep
  their look. All styles render: climate (Earth), patchy, banded,
  overcast.
- **Weather maps** (`Clouds.c`, `cloudmap.frag`): built from each baked
  planet's moisture and wind maps, two per planet crossfaded one sim hour
  apart, flow-mapped along the wind.
- **Slab**, **cloud shadows** on land and water, **stand-in clouds**, and a
  debug panel section (master switch, every setting, detail octaves).

Changes from the plan above, found while building:

- **Heights follow the stylised terrain.** Earth's land relief is 0.025
  radii (~160 km), so a real 5 km layer was buried under every continent.
  Earth's layer is at 80 km (half the relief); generated planets take a
  fraction of their own relief, capped inside their air.
- **Signed coverage field.** The map stores noise minus threshold, and the
  slab thresholds after adding the detail (anti-aliased with `fwidth`).
  Thresholding in the map gave blurred blanket edges.
- **No depth pass.** A depth-only pass to stop the dome at cloud tops left
  haloes wherever its coverage and the colour pass's disagreed. Instead
  the slab adds the dome's in-scatter in front of itself and writes its
  alpha into the scene target's alpha (cleared to 0 each frame in
  `Main.c`; the rings mask it off), and the dome and the sun's glow blend
  with `(ONE_MINUS_DST_ALPHA, ONE)`. That is exact for any alpha.
- **Noise**: the weather map uses hashed gradient noise (value noise showed
  square holes where overcast decks threshold its tails); the slab detail
  uses value noise from a 256² RG8 texture (one bilinear fetch per octave),
  in a program compiled per octave count so the loop unrolls.
- **Flow periods** are short (Earth 12 h; generated 0.25–1.5 days): longer
  ones let the wind belts shear the noise into streaks.

Open issues, for when this resumes:

- **Cost at native resolution** (Iris Xe, 2880×1880, noisy: the laptop
  throttles): ~10 ms with the whole planet in view, ~11–25 ms with clouds
  filling the screen. The detail octaves are most of it (texture-fetch
  bound, ~13 ms full-screen at 9 octaves); the atmosphere terms ~6 ms.
  The edge-band skip (detail only where it can change coverage) did not
  measurably help under the throttling and should be re-measured on a cool
  machine. The real lever is step 3 (reduced resolution + temporal).
- **Stair-stepped fringes where terrain rises through the layer.** Seen on
  generated planets whose relief is far above their layer (seed 2's
  Venus: layer 45 km, relief ~160 km). Confirmed by drawing the slab
  without its depth test: the slab plane is cut by the terrain, and the
  cut is aliased (one depth per pixel against MSAA terrain). Fix options:
  keep generated layers above most of the relief, fade the slab's alpha
  by its height above the terrain, or move to the volume (step 2).
- **Stand-in clouds** only checked at a few pixels; the switch from
  stand-in to full detail is not yet compared side by side.
- **Bench references**: clouds are off by default, so the bench is
  unaffected for now; turning them on by default will need new references.
- **Header dependencies**: fixed 2026-10-22 (refactor phase 2). The
  Makefile now tracks headers, so editing `WorldConfig.h` rebuilds
  everything that includes it.

## References

1. A. Schneider and N. Vos. "The Real-Time Volumetric Cloudscapes of
   Horizon: Zero Dawn." *Advances in Real-Time Rendering in Games*,
   SIGGRAPH 2015 course. Weather map, Perlin-Worley shape noise, height
   profiles, powder term, 4×4 temporal update.
2. A. Schneider. "Nubis: Authoring Real-Time Volumetric Cloudscapes with
   the Decima Engine." *Advances in Real-Time Rendering in Games*,
   SIGGRAPH 2017 course. Production refinements of [1]: cloud types,
   authoring, performance.
3. S. Hillaire. "Physically Based Sky, Atmosphere and Cloud Rendering in
   Frostbite." *Physically Based Shading in Theory and Practice*,
   SIGGRAPH 2016 course. Energy-conserving integration, dual-lobe phase,
   temporal reprojection for clouds.
4. F. Högfeldt. "Convincing Cloud Rendering: An Implementation of
   Real-Time Dynamic Volumetric Clouds in Frostbite." Master's thesis,
   Chalmers University of Technology, 2016. A detailed walkthrough of
   implementing [1] and [3].
5. S. O'Neill. "Accurate Atmospheric Scattering." *GPU Gems 2*, ch. 16,
   Addison-Wesley, 2005. The atmosphere this project already uses.
6. K. Perlin. "Improving Noise." *ACM Transactions on Graphics* 21(3)
   (SIGGRAPH 2002).
7. S. Worley. "A Cellular Texture Basis Function." *Proceedings of
   SIGGRAPH 1996*.
8. R. Bridson, J. Hourihan, M. Nordenstam. "Curl-Noise for Procedural
   Fluid Flow." *ACM Transactions on Graphics* 26(3) (SIGGRAPH 2007).
9. I. Georgiev and M. Fajardo. "Blue-Noise Dithered Sampling."
   SIGGRAPH 2016 Talks.
10. L. G. Henyey and J. L. Greenstein. "Diffuse Radiation in the
    Galaxy." *Astrophysical Journal* 93, 1941.
11. M. Wrenninge, C. Kulla, V. Lundqvist. "Oz: The Great and Volumetric."
    SIGGRAPH 2013 Talks. Multiple-scattering octave approximation.
12. B. Karis. "High Quality Temporal Supersampling." *Advances in
    Real-Time Rendering in Games*, SIGGRAPH 2014 course. Neighbourhood
    clamping for history rejection.
13. J. Kopf, M. F. Cohen, D. Lischinski, M. Uyttendaele. "Joint Bilateral
    Upsampling." *ACM Transactions on Graphics* 26(3) (SIGGRAPH 2007).
14. S. Hillaire. "A Scalable and Production Ready Sky and Atmosphere
    Rendering Technique." *Computer Graphics Forum* 39(4) (EGSR 2020).
15. F. Bauer. "Creating the Atmospheric World of Red Dead Redemption 2:
    A Complete and Integrated Solution." *Advances in Real-Time
    Rendering in Games*, SIGGRAPH 2019 course.
16. A. Vlachos. "Water Flow in Portal 2." *Advances in Real-Time
    Rendering in Games*, SIGGRAPH 2010 course. Two-phase flow maps.
