# Terrain look: detail, baked slope + AO, ground textures

Status: implemented 2026-10-21, uncommitted. Builds on notes/cube-maps.md.

## 1. Detail noise follows the map

- The coarsest octave is one map texel long (lattice level
  log2 TERRAIN_MAP_SIZE), so the noise starts where the map stops.
- Amplitude: the map's RMS height step between neighbouring land texels
  (`PlanetGenParams.landStepRms`, measured in the bake; in metres at the
  current `landRelief`) × `TERRAIN_DETAIL_ROUGHNESS` (debug panel
  "Detail roughness"). Earth seed 1337: 0.00527 raw ≈ 1.4 km.
- Local scale (`detailEnvelope`): the map's slope at the point (from the
  surface map), divided by that RMS, clamped to
  [`TERRAIN_DETAIL_PLAINS`, `TERRAIN_DETAIL_LOCAL_MAX`]. Steep ranges get
  rugged detail; plains get little. The old altitude-based envelope is
  gone. quad.vert and depth.vert read the surface map at the vertex mip,
  so the casters match the surface.
- Bounds and skirts use the maximum local scale
  (`TerrainDetail_Amplitude`, `_Seam`).

## 4 + 5. Surface map (TerrainGen.c `surfacePixels`)

- A new RGBA16F cube map with mips:
  - RGB: the height field's slope as a body-local tangent vector, in raw
    height units per radian. It is 3D, so seamless filtering and mips stay
    correct across face edges.
  - A: horizon-based ambient occlusion. 8 directions × samples at 1–32
    texels over the relief (body radii, sphere curvature included); the
    sky fraction is 1/(1+tan²).
- Baked with the style's relief at bake time (re-bake after changing
  `landRelief`), one thread per face: about +0.2–0.5 s.
- quad.frag reads one texel instead of 4 height differences. The slope now
  filters smoothly instead of showing the height texels.
- AO (debug panel "Terrain AO", `g_terrainAO`) dims the skylight fully
  and the sun by `TERRAIN_AO_SUN` (0.6), a cavity term that reads as depth
  from orbit where shadow maps are too coarse.

## 3. Procedural ground textures (TerrainDetail.c, shaders/groundtex.*)

- Four tileable layers (rock, ground, sand, snow), baked once at startup
  into a 512² RGBA16F 2D array with mips: RG = slope per tile, B = albedo
  factor, A = height. Periodic gradient noise, ridged rock with strata,
  sand ripples, snow drifts.
- Placed on the detail noise's integer lattice. A tile is one lattice
  cell, and the uv is (cell & 63) + fraction, so it is exact at any
  distance on an Earth-size planet. Two lattice levels are blended by
  distance (texels 1–2 px and 2–4 px), so tiling never shows and nothing
  pops.
- Layer weights come from the existing material masks (steep → rock,
  beach → sand, snow → snow, the rest → ground); none on water.
- Fades in below 300 m/px and is full at 40 m/px (`GROUND_FADE`); debug
  panel "Ground textures".
- Known: the tile orientation changes across the 12 cube edges, so a seam
  is visible there up close.

## Cost (same build, alternating runs, planet pass ÷ shadow pass to cancel thermal drift)

- Baked slope vs the old 4-tap differences: within noise.
- Ground textures: ~8 % of the planet pass near the ground, 0 from orbit.
- Bake: surface map +0.2–0.5 s.
