# Procedural moons (scoping)

Status: 2026-10-22. Scoping only, nothing built. This note covers removing the
hand-made Moon and giving every planet n procedural moons, drawn with the same
quadtree LOD terrain as the planets (no rings).

## What exists today

- **`BODY_MOON`** is a fixed entry in the `BodyId` enum (`bodies.h`), with
  `BODY_KIND_TEXTURED`, fixed `MOON_*` constants in `WorldConfig.h`, and Earth
  as its parent.
- **Drawing** (`BodyDraw_Moon`, `body_draw.c`): a smooth quadtree sphere
  (`relief = NULL`) through `light.vert/light.frag`, with `moon.jpg`
  equirect-mapped (the `instanced` path in `light.vert` exists only for it).
- **Shadows:** in `render.c` the Moon is a mesh caster (`quadCubeVAO`) into the
  global map and every cascade. The global light box's depth is stretched to
  the Moon's orbit (`reach`) so the Moon and Earth can eclipse each other. Earth
  only.
- **Stand-in:** `Planets_Draw(..., moonFull, ...)` special-cases it.
- **Other hooks:** `app->moonTex`, `PROF_MOON`, the `moonFull` gate.

Everything else about orbits already works for moons: `Body.parent` is
general, `Bodies_Update` updates parents first, and `semiMajor` compresses only
the Sun's children, so moon distances stay real.

## What blocks n procedural terrain moons

### 1. The body table is a fixed enum

`BODY_COUNT` sizes `g_bodies`, `g_planetInfo`, the clouds' `maps[]`, Tab
cycling (`app.c`), the focus buttons, bench `pivot <name>` and the
`Planets_Draw` loops. Moons need a table that grows at generation time.

- **Change:** keep the named planet ids (seed slots), add `BODY_MAX` (e.g. 64)
  and `Bodies_Count()`. Moons are appended after all planets, so parents still
  come first. Loops go to `Bodies_Count()` and arrays to `BODY_MAX`.
- **Names:** "Jupiter I", "Jupiter II", ... so that bench `pivot` still works.
- **Size:** small and mechanical, about 10 sites.

### 2. Only one terrain body can be drawn in full (the main job)

The active-planet design keeps one planet in the globals
(`TerrainGen_GenParams/Style/BakeSettings`, `TerrainDraw_HeightTex`,
`Atmosphere_Params`, `Clouds_Params`) and swaps them on activation. Moons need
to be drawn **at the same time** as their planet:

- From Earth's surface a Moon-sized moon is about 6 px in radius at 1080p
  (FOV 45°). That is above `FULL_MIN_PX` (3), so it is drawn in full today.
- From such a moon, its planet is about 23 px in radius, with atmosphere,
  clouds and water visible.

The terrain draw path reads the globals throughout:

- `reliefOf` and `terrainReliefBounds` read the active planet.
- `QuadTree_ReliefFn` has no context pointer.
- `TerrainGen_HeightRange` and `GlobalRange` query the active pyramid.
- `TerrainGen_SetSurfaceUniforms` uploads the active style and maps.
- `terrainReliefBounds` looks up `SolarSystem_ActiveBody()` for the radius.

**Recommendation: an "active system" rather than an active planet.**

- **One draw handle per planet.** Add a `TerrainPlanet` handle (body,
  `PlanetGenParams*`, `PlanetStyle*`, height texture, map size). Pass it through
  `TerrainDraw_Body/PlanetBody/Planet/ShadowCasters/ViewRange`. Add a `void *ctx`
  to `QuadTree_ReliefFn` / `QuadTree_Body`. Add `TerrainGen_PlanetHeightRange`
  (the `PlanetRange` pattern that already exists) and give
  `TerrainGen_SetSurfaceUniforms` a planet argument.
- **What "active" means after that.** The globals stay the active planet, and
  only matter for which one the debug panel edits. Each draw takes its handle
  explicitly. The active planet's handle points at the globals, so Earth renders
  exactly as now.
- **The active system is a planet plus its moons.** Atmosphere, clouds and water
  stay single, tied to the parent planet. This works because moons are airless
  and dry to start with. `SolarSystem_PickActive` maps a moon to its parent, so
  flying to a moon keeps its planet active and drawn with its atmosphere.
- **Check:** Earth must stay pixel-identical (`visual_check.ps1`) before any
  moon is drawn.

### 3. A moon terrain recipe

`heightbake.frag` shapes tectonic plates, which is wrong for moons.

- **Shaping:** a crater-field mode, either a `#define` variant of
  `heightbake.frag` or a `moonbake.frag` that reuses the strip / PBO / cube-map
  plumbing in `gen/bake_job.c`:
  - power-law crater sizes;
  - bowl, rim, ejecta and central-peak profile, with large craters flattened
    into basins;
  - maria as a few large flooded basins (low, flat, dark);
  - optional rays later.
- **Erosion:** no hydraulic droplets (no air), a few thermal passes only. That
  makes the bake much faster than a planet's (droplets dominate bake time).
- **Maps:** no climate or ocean map. Bind 1×1 dummies; humidity is already 0
  for dry worlds. Keep the surface map (slope and AO), because crater AO is most
  of the look.
- **Craters below a map texel:** add a cellular crater term to `detail.glsl` for
  moons. At 512²/face on a 1,700 km moon, one texel is about 7 km.
- **Look in `quad.frag`:** an airless switch with no extinction, a black sky
  term, and a harder terminator (Lommel-Seeliger-like). Style is the existing
  `PlanetStyle` palette (greys, with tinted ices for outer moons).

### 4. Memory and map size

At `TERRAIN_MAP_SIZE` 1024, one baked planet uses roughly this much GPU memory
(with mips):

| Map | Format | Size |
|---|---|---|
| Height | R32F | 33 MB |
| Climate | RGBA8 | 33 MB |
| Ocean | RGBA8 | 33 MB |
| Surface | RGBA16F | 67 MB |
| **Total** | | **~165 MB** |

Many moons at that size would not fit on the Iris Xe (shared memory).

- **Moon size:** moons at 512 with height and surface maps only come to about
  25 MB each.
- **Map size per planet:** `TERRAIN_MAP_SIZE` is compile-time in 11 places,
  including `quad.frag` and the inline `TerrainGen_MapTexelRad`, so it has to
  become a per-planet value (a uniform plus a `PlanetGenParams` field).
- **Bake policy:**
  - Bake only the moons of the active system, its largest moons first.
  - Keep the existing one-at-a-time background slot.
  - Free a system's moon bakes when another system becomes active (LRU,
    keeping the last 1 or 2 systems).

### 5. Shadows and eclipses

Today Earth's global shadow map is stretched to the Moon's orbit. That does not
scale to n moons, or to outer moons millions of km out.

- **Recommendation:** analytic sphere eclipses. The receiver shaders
  (`quad.frag`, `water.frag`, `atmosphere`, the stand-in) get up to N occluder
  spheres (the planet plus its nearest moons) as uniforms. They compute
  sun-disc coverage, which also gives soft penumbrae.
- **Shadow maps:** only for terrain self-shadowing, of the body nearest the
  camera. The `reach` hack and the moon mesh caster go away.
- **Cascades:** fit around the body nearest the camera, not always the active
  planet (`TerrainDraw_ViewRange` takes the handle).

### 6. Orbits and generation (`solar_system.c`, ranges in `WorldConfig.h`)

- **Count, on its own RNG stream per planet** (adding moons must not reshuffle
  existing planets):

  | Planet type | Moons |
  |---|---|
  | Rocky | 0–2 |
  | Dwarf | 0–1 |
  | Gas giant | 2–8 |

- **Size:** dwarf to Moon/Ganymede scale, 200–2,600 km, at most about 0.3 × the
  parent's radius (gas giants, much less).
- **Distance:** from just outside `RING_OUTER` × R (or 3 R without rings), out
  to about 60 R for regular moons. One irregular outer moon can sit further out,
  inclined or retrograde.
- **Spacing:** log-spaced, like the planet slots.
- **Period:** Kepler's third law around the **parent**. `orbitPeriod` assumes
  the Sun today, so it needs the parent's GM. Derive that from radius times a
  density per type (new `WorldConfig.h` constants), or store a generated period.
- **Spin:** tidally locked by default (`rotationPeriod` = orbital period), tilt
  about 0.
- **Generated look:** a crater density and maria fraction per moon, plus an
  ice/rock palette by distance from the Sun (reuse `tempOffset`).
- **Hill sphere caveat:** with `ORBIT_DISTANCE_SCALE` 0.1, planets' Hill spheres
  shrink tenfold, and Earth's Moon already sits outside its compressed one.
  There is no n-body simulation, so nothing breaks. It is only worth knowing if
  a "realistic" limit is ever wanted.

### 7. Stand-ins, camera and UI

- **Stand-ins:** these come almost for free. Moons are terrain bodies in the
  table, so `Planets_Draw` draws them as stand-ins until they are a few pixels
  across. The existing stand-in-to-full colour pop applies to moons too.
- **Camera and UI:**
  - Tab cycles moons as well (or Tab cycles planets and another key cycles the
    current planet's moons).
  - The focus panel groups moons under their planet.
  - Bench gets `pivot "Earth I"`.

## Removing the old Moon (independent first step)

Things to delete:

- `BODY_MOON`
- the `MOON_*` constants
- `moon.jpg` loading and `app->moonTex`
- `BodyDraw_Moon` and the `instanced` path in `light.vert`/`light.frag`
- the moon caster and `reach` in `render.c`
- the `moonFull` parameter of `Planets_Draw`

`PROF_MOON` becomes "Moons". No bench scene names the Moon. If the Moon appears
in any reference shot (Earth sky scenes), those refs change and need your
approval (`-UpdateRefs`).

## Suggested order

1. **Body table and generated moons as stand-ins only.** Remove the old Moon,
   make the body table dynamic, add moon orbits, sizes and tidal lock, and handle
   focus, Tab and bench names. Moons are visible and orbiting, with no terrain
   yet. Small.
2. **Remove the terrain singleton from the draw path.** Add the `TerrainPlanet`
   handle, the relief context and per-planet map size. Earth must stay
   pixel-identical and bench must hold. The largest and riskiest step.
3. **Moon bake.** Crater shaping, the airless pipeline, 512 maps, skipped
   climate and ocean maps, and the bake and evict policy.
4. **Draw the active system.** Draw the parent and its moons in full, map
   `PickActive` from moons to parents, add analytic eclipses, and fit cascades
   around the nearest body.
5. **Look.** Sub-texel craters in `detail.glsl`, maria albedo, airless shading,
   and stand-ins that sample the baked maps.

## Open questions

- **Earth:** should it get generated moons from the system seed like everyone
  else, or one fixed large moon (procedural terrain, Moon-like orbit) so its sky
  keeps a familiar moon?
- **Gas giants:** do they get moons in the first pass (most moons in a real
  system orbit gas giants)?
- **Maximum moons per planet** (memory and Tab-cycle length)?
- **Later:** moons with atmospheres or water (Titan, Europa-like), or always
  airless for now?
