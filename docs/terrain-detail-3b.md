# Step 3b: height detail near the ground

Status: **implemented** 2026-10-21 (design below, written 2026-10-20;
what was built and what changed from the plan is at the end).

## Where things stand

- The quadtree now subdivides to ~1 m vertex spacing (about level 20 on
  Earth; `QT_MIN_VERTEX_SPACING_M` in `src/WorldConfig.h`), and vertex
  positions are accurate to well under a millimetre near the camera
  (double-precision patch corners, `src/shaders/patch.glsl`).
- But the only height source is the global baked map: 2048×1024 R32F,
  about **20 km per texel** at Earth scale (`generatePlanetHeightmap` in
  `src/TerrainGen.c`). Below level ~10 the extra triangles just trace that
  map's smooth bilinear surface, so the ground looks no more detailed up
  close.

3b adds real height detail below the map's resolution.

## Approach (recommended first): procedural noise in the vertex shaders

Add fractal noise octaves on top of the global map, fading in as the
camera gets close.

### Precision-safe noise domain

Feeding the float sphere direction into noise breaks down at ~1 m features
(a float direction resolves ~1e-7 rad ≈ 0.6 m on Earth). Instead:

- Evaluate **3D lattice noise at the cube-surface point** q (not the
  sphere point). q is shared exactly by neighbouring patches and by the
  six faces along cube edges, so the noise is continuous everywhere with
  no face seams.
- q is exact from integers: q = O_face + (x + u)·size·A + (y + v)·size·B,
  with size = 2^-level (`cubePoint` in `src/geometry/QuadTree.c`). For an
  octave with lattice spacing 2^-n (cube units):
  - lattice cell = integer part, computed from the patch's integer
    `(x, y)` shifted by `(n - level)` plus the face origin (exact integer
    maths in the shader, `int`/`uint`);
  - position inside the cell = a small float from `(u, v)` scaled by
    `2^(n - level)` (always small, so precise).
- The cube-to-sphere mapping stretches cells by up to ~1.4×,
  anisotropically near cube corners. That's acceptable for detail noise.

### Data the shaders need

Patches currently carry `pC0..pC3`, `pQ0`, `pEu`, `pEv` (26 floats,
locations 4–10). Add the node's integer address as an integer attribute
(location 11, `glVertexAttribIPointer`): `ivec4(face, level, x, y)`.
`x`/`y` fit in int32 up to level 30. Fill it in `emitLeaf`.

### Amplitude and fade

- Octave amplitudes in metres, from config, e.g. `TERRAIN_DETAIL_*` in
  `WorldConfig.h` (base amplitude, octave count, lacunarity ~2, gain ~0.5).
- **Fade octaves by vertex distance to the camera, not by patch level.**
  Distance is the same for a vertex shared by two neighbouring patches,
  so there are no cracks and no popping when a patch splits. An octave
  should be fully in once its wavelength spans several pixels, and gone
  before it aliases (same footprint idea as `lodPix`).
- Scale by terrain character from the existing maps: rougher in mountains
  (height-map slope), smoother on plains and beaches, using the climate
  map's biome / alluvium channels.
- **Coastline:** noise must not push land below sea level or seafloor
  above it near the coast. water.frag and quad.frag decide land vs sea per
  pixel from the global map only, so crossings would mismatch the shore.
  Fade the noise to zero within a band above sea level, and keep it off
  the seafloor (or seafloor-only with a separate mask).

### Where the code goes

- One function in `src/shaders/patch.glsl` (e.g. `patchDetailHeight(uv)`
  returning metres), used by `quad.vert` and `depth.vert` so the planet
  and its shadow casters get identical heights. Convert to body radii by
  dividing by R, then add to `lift` like the existing relief.
- `water.vert` only needs it if the detail is allowed near the coast; with
  the coastal fade above it does not.
- **Bounds:** `earthReliefBounds` in `src/Main.c` must widen `[lo, hi]` by
  the maximum noise amplitude that can be active over the cap, or culling
  and the split distance go wrong. The skirt drop (`skirtDrop` in
  QuadTree.c) grows with it automatically since it uses the range.
- **Shading:** quad.frag's per-pixel normal comes from the height-map
  gradient plus float-direction noise (`vnoiseLod(dObj, ...)`), which
  cannot resolve sub-metre detail. Pass the patch's integer address as a
  `flat` varying and add the analytic gradient of the same noise octaves
  to the shading normal, using the same lattice maths.

### Costs to watch

- Every planet vertex is shaded twice (depth prepass + colour pass) plus
  in the shadow-caster pass. Noise octaves × hashes per vertex add up;
  keep the octave count modest and skip octaves that are fully faded.
- The shadow walk is coarser (`QT_SHADOW_TRIANGLE_PX`), so casters carry
  less detail than receivers. The receiver offsets in `sunVisibility`
  cover ~3 km shadow texels today; with cascaded shadows (step 4) this
  needs revisiting.

## Later option: GPU height tiles per node (Proland-style)

When a node first appears, render its own height (and normal) texture on
the GPU, keep it in a texture array or atlas, and evict by LRU. Children
start from an upsampled copy of their parent's tile plus new noise.

- Gains: per-pixel normals at any depth, erosion-style filters, and
  detail that costs nothing per frame once generated.
- Costs: a tile cache, a per-frame generation budget (to avoid hitches),
  and tile seams to handle (borders shared between neighbours).
- The noise function from the first approach becomes the tile generator,
  so it is not wasted work.

## Related constants that will matter at ground level

Not part of 3b, but they decide how the detail reads (step 4):

- `landRelief` 0.025 R ≈ 160 km mountains (Everest is ~0.0014 R) and
  `oceanRelief` 0.01 R ≈ 64 km ocean depth (`g_planetStyle`,
  `src/TerrainGen.c`, debug panel). Realistic values will make the
  detail noise the dominant small-scale relief.
- The shadow map's texels are ~3 km at the planet, so ground-level
  shadows are blobs until cascaded shadow maps.

## Verification

- Extend the precision harness idea (CPU emulation of the shader maths
  against a double ground truth) to the noise: the same vertex computed
  from two neighbouring patches (and across a cube edge) must match to
  float precision.
- Fly to a few metres altitude with wireframe (T): no cracks at patch
  edges, no popping as patches split, no shimmer while the planet spins.
- Debug panel Camera section: patch counts and walk time should barely
  change (bounds widen a little); Performance section: planet and shadow
  GPU time for the added vertex work.
- Check coastlines at low altitude for land/sea mismatches.

## As built (2026-10-21)

Files: `src/shaders/detail.glsl` (noise, fades, slope maths),
`src/TerrainDetail.c/.h` (octave setup, gradient table, bounds),
`TERRAIN_DETAIL_*` in `src/WorldConfig.h`, patch address at attribute
location 11 (`QuadTree.c` `emitLeaf`, `patch.glsl` `pAddr`), detail added to
`lift` in `quad.vert` / `depth.vert`, slope in `quad.frag`, debug panel
checkbox "Terrain detail noise" (Camera section).

Changes from the plan:

- **No integer hash.** Corner gradients come from a 64^3 random gradient
  table stored as a 2D array texture (once per constant cube axis), read
  with two `textureGather`s per octave. 32-bit integer multiplies are
  emulated on Intel Xe; the hash version cost ~35% more. Each octave
  shifts the table so octaves do not line up.
- **Cube edges:** corner gradients drop their component across a cube
  edge, so the field is C1 there (no crease), not just continuous.
- **Loops run fine to coarse** (`detailCoarsen`): stepping the lattice
  coarse to fine doubles the fraction's rounding error every octave.
- **Per-pixel octaves are off by default**
  (`TERRAIN_DETAIL_PIXEL_OCTAVES 0`, compiled into quad.frag). Measured
  with land filling a 2880x1880 screen: vertex detail ~+5 ms; per-pixel
  ~+9 ms for the first octave, ~+4 ms per further one (band holds 3). Most
  of the cost is register pressure in the big planet shader, not the
  noise maths: the loop sits before the material code for that reason.
  With 0 the vertices still carry the slope of every octave they
  resolve (interpolated), so shading matches the geometry.
- **Bounds and skirts:** the walk widens land bounds by the full detail
  amplitude (`QuadTree_Body.detailAmp`) but sizes skirts from the coarse
  range plus a seam term (`detailSeam` x patch width), so skirts do not
  grow by the amplitude.
- No climate-map roughness: amplitude follows altitude only
  (`TERRAIN_DETAIL_PLAINS` on low land), plus the coastal ramp.

Verified: CPU float emulation of the noise (seams between patches,
parent/child corners and across cube edges <= 5e-7 of an octave's
amplitude, slope across cube edges ~0, |noise| <= 0.81 < the bound of 1,
fine-to-coarse stepping matches direct evaluation). bench.ps1 and
visual_check.ps1 pass.

Not yet verified by eye: the look at low altitude. The atmosphere is still
sized in body radii (step 4), and within a few km of the ground its haze
hides nearly all surface shading, detail included. Revisit the
amplitudes (`TERRAIN_DETAIL_AMPLITUDE_M`, `_PLAINS`) and per-pixel octaves
once step 4 makes the ground visible up close.
