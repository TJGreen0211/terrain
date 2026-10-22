# Baked terrain maps as cube maps

Status: implemented 2026-10-21, uncommitted.

## Why

The baked height, climate and ocean maps were 2048×1024 equirect:
~20 km texels at the equator, and a pinch where every column meets at
the poles. They are now cube maps, `TERRAIN_MAP_SIZE` (1024) texels per
face edge (`WorldConfig.h`).

## Layout

- GL cube map, GL's face order and orientation, plain (gnomonic)
  projection, the same projection as the quadtree (`normalize(q)`).
  A face's texel grid therefore lines up with quadtree nodes. That makes
  per-node tiles (Proland-style, "option 5") a sub-rectangle of a face.
- Shaders look up by body-local direction: no `atan`/`asin`.
  `shaders/planetmap.glsl` has the helpers.
- Texel size: ~12.4 km at face centres on Earth. Toward the corners the
  smallest extent shrinks by m² (m = the direction's major component),
  down to ~4 km. `planetMapLodBias` raises the mip by up to log2(3)
  there. `TerrainGen_HeightRange` adds the same bias before clamping to
  mip 0.
- Seamless cube filtering is on globally. `uploadCube` enables it, and
  Nebula.c no longer turns it off after a sky bake.

## Bake (TerrainGen.c)

- GPU shaping renders the six faces stacked into one N × 6N target.
  Readback row = face·N + j, which is also the CPU field layout.
- Thermal erosion, droplet erosion, drainage, the alluvium blur and the
  bounds pyramid all run on the faces. `cubeIndex` finds neighbours
  across face edges. One texel past an edge it returns exactly the
  adjacent texel, the same one seamless filtering uses. Droplets cross
  face edges (`crossFace`).
- Units: the erosion and climate constants keep their old tuning, per
  reference texel (π/1024 rad, the old equator texel). They are converted
  with the face texels' actual size and spacing: talus per spacing; droplet
  step, lifetime, evaporation, erosion and deposit rates per distance;
  capacity from slope; droplet count = `dropletsPerPx` × 2048 × 1024;
  discharge in reference-texel areas. The erosion brush stays 2 texels,
  so channels sharpen with the map.
- Check: at `TERRAIN_MAP_SIZE` 512 (about the old resolution), lake and
  river area match the old bake (2.9 % / 3.6 % of land vs 3.0 % / 3.6 %).
- Moisture and wave fetch stream along latitude rows. They still run on a
  2048×1024 equirect copy and are resampled onto the cube. They are
  smooth fields, so no pinch shows.
- quad.frag's `VALLEY_LOD`, `LAKE_LOD` and `COAST_LOD` are +0.65 mips,
  so they keep their footprint on the ground.

## Measured (Iris Xe, alternating A/B runs, medians of 3)

| scene | frame old → new | planet GPU old → new | bake old → new |
|---|---|---|---|
| grazing-sea | 22.5 → 22.5 ms | 7.8 → 7.0 ms | 5.6 → 13.7 s |
| orbit-mid | 18.0 → 18.9 ms | 3.6 → 3.2 ms | 4.0 → 15.2 s |
| topdown-detail | 25.1 → 22.6 ms | 11.3 → 9.8 ms | 5.3 → 14.3 s |

Bake is ~3× (mostly hydraulic erosion, 1.6× the steps, plus edge cases).

## Known differences / open

- Lakes: at 1024 the finer grid keeps more closed basins. Lake area is
  ~6 % of land, against 3 % before. Raising `dropletsPerPx` to 0.86 (the
  old density per texel) brings it to 3.9 %, but erosion takes 2.5×
  longer. Left at 0.35.
- Rivers are thinner (sharper up close) and fainter from far orbit
  (2.75 % vs 3.6 % of land above the river threshold).
- visual_check.ps1 fails by design: the terrain changed. References need
  a reviewed `-UpdateRefs`.
- The water's wave textures still use long/lat uv and still pinch at the
  poles. That is separate from the baked maps.
- `TERRAIN_DUMP` / `TERRAIN_DUMP_CLIMATE` now write six faces back to
  back.
