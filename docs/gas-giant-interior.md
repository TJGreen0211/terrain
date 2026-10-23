# Gas giant interior

Status: 2026-10-23. Plan agreed with the user; stage 1 (getting inside)
built; stage 2 (the shared cloud volume) committed; stage 3 (shaping,
lightning, shafts) first pass built, uncommitted (see the "as built"
sections). Follows
the exterior work in `docs/gas-giants.md` and resumes steps 2-3 of
`docs/volumetric-clouds.md` as a shared cloud module.

Goal: fly from space down into a gas giant and among its cloud decks.
The target look is the concept painting "Shadow of Io": creamy, high-key
haze, the Sun glowing through it with soft rays, rust and umber cloud
walls and shelves with swept, sculpted edges and overhangs, and a dark
deep layer with lightning.

## The user's choices

- **Full volumes:** the camera flies into and through the clouds. A
  cheaper layered-surface ("2.5D") approach was considered and rejected:
  it can't do overhangs or flying through a cloud.
- **Depth:** down to a little below the water-cloud deck, where it fades
  to dark murk. Nothing deeper is needed; the point is the big clouds.
- **Performance:** not a blocker. The user will move to a faster machine
  if the Iris Xe can't keep up. Low resolution plus accumulation over
  frames is still the plan (it is how every real-time volume renderer
  works), but quality isn't to be cut to fit the laptop.
- **Shared cloud code:** one volumetric cloud module for gas giants and
  the rocky planets' clouds.
- **Motion:** static first, then cloud layers drifting at different
  speeds. Shape change (billowing) and moving bands seen from outside are
  later, if ever.
- **Time:** cloud motion follows simulation time. The app now starts in
  real time (`SIM_TIME_SCALE` 1.0, done 2026-10-22); the panel's slider
  and its "1 day / min" button speed it up. Bench runs keep the old
  1440x (`BENCH_TIME_SCALE`), so the bench scenes are unchanged.

## What exists

- **Gas giants are stand-ins only:** a ray-traced sphere shaded by
  `gasgiant.glsl` (`docs/gas-giants.md`). `planets.c` skips a body once
  the camera is inside its radius, so flying in shows empty space.
- **One terrain planet is drawn in full** (`SolarSystem_ActiveBody`); the
  atmosphere dome (`world/atmosphere`) is built around it. Gas giants are
  never active.
- **Clouds:** step 1 (a 2D slab) is built and switched off by default.
  The volume (steps 2-6 of `docs/volumetric-clouds.md`) was designed but
  never built.
- **Camera:** an orbit camera around a pivot with fixed, slider-set speeds
  (1 m/s to 6.7 Earth radii/s) and a surface mode for the terrain planet.
- **Time:** simulation seconds per real second, live in the Solar system
  panel.

## Scale

A gas giant is 22,000-75,000 km in radius (`GAS_RADIUS_*_M`). Jupiter's
real decks, relative to the 1 bar level:

| Deck | Pressure | Altitude | Look |
|---|---|---|---|
| Upper haze | < 0.3 bar | +50 km and up | thin, warm veil |
| Ammonia ice | 0.5-0.7 bar | +15 to +30 km | bright, the face seen from space |
| Ammonium hydrosulfide | 1.5-2 bar | -10 to -25 km | rust, ochre, umber |
| Water | 5-7 bar | -50 to -70 km | towering, dark, lightning |

The pressure scale height is about 27 km. The painting's cloud walls are
far taller than these thin decks, so the layers get a vertical
exaggeration setting. The layer altitudes and thicknesses go in
`WorldConfig.h` (metres, scaled per giant by its scale height), next to
the other `GAS_` constants.

Precision: in planet-local floats at 7.5e7 m, one float step is ~8 m.
That is fine for weather (tens of km) but not for detail noise at metre
scale near the camera, so detail noise is sampled in a camera-anchored
frame (planet-local position minus a re-centred origin computed in
double on the CPU).

## Architecture

- **Shared cloud volume** in `world/clouds/` (resuming the clouds
  module): a raymarch pass at reduced resolution, accumulated over frames,
  then upsampled into the HDR scene. It takes a list of layers per body
  (base altitude, thickness, density profile with height, colour,
  absorption) and a weather source.
- **Weather source per body:**
  - Rocky planets: their existing weather map textures.
  - Gas giants: the exterior band function itself (`gasgiant.glsl`:
    bands, storms, polar cyclones, high clouds), so what you see inside
    matches what you saw from space. It is too expensive to run per march
    sample, so it is baked into a **local weather map** around the camera
    (a tangent-plane texture a few thousand km across, re-centred as the
    camera moves; a coarser global map for the distance).
- **Gas giant interior** in `world/gasgiant/`: the sky, the far deck, the
  haze, the Sun's glow and rays, and the weather bake. It owns the gas
  giant's layer list and hands it to the clouds module.
- **App:** `render.c` gains an interior branch. When the camera is inside
  a giant's shell, that giant's stand-in is replaced by the interior
  passes (sky, far deck, cloud volume, haze). Other bodies are fogged by
  the haze.

## Stages

### 1. Get inside (small)

The first fly-in, with no volumes yet.

- **Detection and hand-off:** a giant is "entered" when the camera is
  below its haze top (`shellFraction`). From there the stand-in gives way
  to the interior passes, cross-faded over an altitude band so there is
  no pop.
- **Sky:** warm, creamy haze in-scatter that thickens with depth, a
  horizon, the Sun's disc with glare and soft rays. Lit in HDR linear
  like the rest of the scene.
- **Far deck:** the cloud top drawn as a vast surface below the camera,
  shaded by the existing exterior function at grazing angles and fading
  into the haze. Later this stays as the far field beyond the volume's
  march distance.
- **Flight:** speed scales with altitude above the deck (fast from
  orbit, slow among the clouds), and a floor below the water deck that
  the camera can't pass.

#### Stage 1 as built (2026-10-22)

- **Module:** `world/gasgiant/gas_interior.c/.h` (`GasInterior_`), with
  `shaders/gasinterior.vert/.frag`. Its sliders are the "Interior" group
  at the bottom of the Gas giants tab (on/off, skylight, sun rays, sun
  glare, murk density). Profiler section "Gas interior".
- **Far deck:** the stand-in sphere itself. Up close it draws over the
  whole screen (a billboard can't cover the silhouette cone), and it stops
  once the camera is below the cloud tops. Fixes it needed up close (none
  changes the view from farther out):
  - the noise footprint is a pixel at the hit point, not at the centre's
    distance;
  - the edge blend is a pixel where the ray passes the limb (with the
    centre's distance, every ray below the horizontal counted as a hit
    near the cloud tops, raising the horizon and cutting the haze short);
  - within 0.05 radii of the cloud tops the detail fbm runs up to 8
    octaves instead of 5 (they still fade out below a pixel).
- **Haze** (`gasinterior.frag`): a full-screen march (32 steps) from the
  camera to the scene depth or the haze top, blended premultiplied over
  everything after the Sun's glow.
  - Density `sigma0 * exp(-a / H)` above the cloud tops. H is the glow
    shell (`shellFraction * R`) over `GAS_HAZE_TOP_H` (12), so the haze
    fills the same shell the glow outside shows. sigma0 is `hazeTau / H`,
    so the column matches the stand-in's veil.
  - Lit like the veil: wrap light reddened toward the terminator, the
    same Henyey-Greenstein lobe, plus skylight; the Sun's light comes
    through the haze above (Kasten-Young air mass).
  - Soft rays: broad streaks by angle around the Sun (in a frame tied to
    the local vertical), none right at the Sun; a narrow aureole
    (HG g = 0.9) for the glowing core.
  - By day, haze over the sky hides the stars and nebula behind it.
  - Below the cloud tops: murk (`GAS_DECK_MURK_PER_M`) in the belt
    colour, lit by sunlight dying away with depth. The march takes even
    steps through the murk before the haze beyond (quadratic steps
    there made visible grain).
- **Hand-off:** between 8 and 12 scale heights the stand-in's glow beyond
  the limb fades out (it assumes the camera is outside the shell). Between
  4 and 8 the interior pass fades in while the stand-in's veil and limb
  darkening fade out (`gasInside`). It is continuous from orbit to the
  cloud tops.
- **Flight** (`app.c`): near a giant the WASD speed scales with altitude
  above its cloud tops (`GAS_FLIGHT_*`; full speed from 20,000 km, at
  least 2e-4 of it, ~850 m/s at the default slider). The camera never goes
  below `GAS_INTERIOR_FLOOR_M` (120 km) under the cloud tops
  (`Camera_KeepOutside`). Engine side: `Camera_SetSpeedScale`.
- **Bench:** a `localup` scene token levels the horizon for subsolar
  pivots (`Camera_SetOrbitEyeDirUp`). Scratch scenes for this work used
  `pivot subsolar <giant>`, `localup`, `poff <altitude>` and `phase`.
- **Checks:** clean build at 127 warnings; the 12 bench reference shots
  are pixel-identical to before. They already failed `visual_check`
  before this work (the HDR change still needs a re-baseline).
- **Cost:** the haze pass is ~5 ms at 1280x720 on the Iris Xe. Room to cut
  it: half resolution, cheaper air mass, fewer steps for rays that end
  close.
- **What it looks like:** a creamy haze sky, a soft glowing Sun with broad
  rays, a bright hazy horizon, the bands as soft stripes on the cloud
  floor, murk below. The cloud floor has little fine detail up close;
  that is stage 2's job.

### 2. Volumetric clouds (large: most of the work)

Steps 2-3 of `docs/volumetric-clouds.md`, built as the shared module.

- **Layers:** ammonia (bright, thin), ammonium hydrosulfide (rust), water
  (tall, dark), each with its own density profile with height.
- **Density:** the weather map's coverage, shaped by layer profiles, then
  eroded by 3D detail noise.
- **March:** ray against the layer shells; larger steps through empty
  space; jittered starts; fewer steps with distance.
- **Lighting:** a short march toward the Sun with Beer-Lambert, the
  energy-conserving integration from the clouds plan, a multiple-scatter
  approximation (softens and brightens the bodies, the painting's glow),
  forward-scatter silver lining, and ambient from the haze above and the
  deck below.
- **Haze:** the clouds sit inside the haze, so they fade with distance in
  the same warm in-scatter as the sky.
- **Resolution:** reduced, accumulated over frames by reprojection, then
  upsampled with depth awareness. Quality settings stay runtime
  adjustable.
- **Rocky planets:** the same module later replaces their 2D slab (not
  part of this work, but nothing in it is gas-giant specific).

#### Stage 2 as built (2026-10-22, first pass)

- **Shared module:** `world/clouds/cloud_volume.c/.h` (`CloudVolume_`),
  shaders `cloudvol.vert`, `cloudvolume.frag` (march), `cloudresolve.frag`
  (temporal), `cloudcomposite.frag`. It knows nothing about gas giants: a
  provider hands it a `CloudVolumeScene` (layers, weather map frame, the
  Sun and the air above). Its settings are the "Volume (shared)" group of
  the Clouds tab; profiler section "Cloud volume". The old 2D slab in
  `clouds.c` is untouched (the clouds module was never finished; the
  volume doesn't depend on its unfinished parts).
- **Weather map:** 1024² (two RGBA8 textures: top cover, middle cover,
  towering; albedo), a tangent-plane patch `GAS_CLOUD_MAP_EXTENT_M`
  (2,000 km) wide under the camera in the body's frame. The provider bakes
  it with its own pass. Gas giants: `gasweather.frag` runs the exterior's
  cloud field, now split out of `gasGiantSurface` as `gasCloudField` (a
  pure refactor: the outside view is pixel-identical). Top cover follows
  the zones plus the high clouds; middle cover is everywhere (0.5..1),
  more where the bands churn and around storms; towering follows the
  turbulence and storms; the albedo is the band colour. It re-bakes when
  the camera's ground point moves an eighth of the map, or the style
  changes.
- **Noise:** built on the CPU at startup (OpenMP): 128³ shape
  (Perlin-Worley + three Worley FBMs) and 32³ detail (three Worley FBMs),
  tiling. The Perlin-Worley channel only spans ~0.5..0.95, so the march
  stretches it over that range.
- **Layers (gas giants):** in haze scale heights. Middle (rust, 0.08-0.55
  H, scale 30 km, density 1/500 m) and top (bright, 0.62-0.85 H, scale
  90 km, swept 3x, density 1/4000 m). Albedo = map colour x tint.
- **Density:** the HZD recipe. The map's cover, swung by a broad noise
  (~5 shape periods, swept east-west: thick regions and wide valleys, and
  it hides the tiling) shapes the Perlin-Worley under a height profile.
  Tops are domes: two cell noises turned from cones into circle profiles,
  as high as the map's towering. Worley detail erodes the edges.
- **Light:** a 5-step march toward the Sun; two HG lobes; three
  multiple-scattering octaves (Wrenninge), each picking up the albedo once
  more (warm, rich shadows); powder; sky- and ground-light by height in
  the shell. The Sun comes through the haze above (the stage 1 model).
- **Air in front:** the march fogs its clouds with the same exponential
  haze to their weighted distance, and is drawn after the stage 1 haze
  pass. (Writing cloud depth for the haze pass instead gave hard, jagged
  outlines where the haze jumped from the cloud's distance to the
  background's.)
- **Resolution and accumulation:** half resolution, sub-pixel jitter
  (Halton 2,3), reprojection in the body's frame with a 3x3 neighbourhood
  clamp (history 0.9), bilinear upsample, premultiplied over the scene.
- **Hand-off:** the volume fades in between 11 and 7 scale heights
  (`GAS_CLOUD_FADE_*_H`; it first fade in at 5..3, which left too much
  plain haze on the way down). From that high the layers are out of the
  march's usual reach, so the reach grows with the camera's height over
  them (`CloudVolume_Reach`: 2.5x, capped at 0.45 of the weather map).
  The stand-in deck darkens under the clouds only within that reach
  (`gasDeep`, `gasDeepReach`, fading over the same last 40% as the
  clouds), so beyond it the deck keeps its outside look and the clouds
  blend into it instead of ending in a patch. It also hides the
  stand-in's own high clouds there.
- **Matching colours:** the air in front of the clouds and their sky- and
  ground-light take display colours and convert them with the exact
  inverse of the curve, as the haze pass does (an approximate `pow 2.2`
  made the clouds grey-green against the deck). The direct sunlight is
  doubled (thick cloud reflects about like the deck's sunlit Lambert
  surface), and each scattering order takes the albedo's hue, not its
  darkness: with the albedo itself, side- and front-lit clouds read
  darker than the deck below them.
- **Cost (Iris Xe, 1280x720):** the cloud volume ~16 ms above the clouds
  and ~31 ms inside them, on top of the ~8 ms haze. Room: empty-space
  skipping (big steps where the map has no cover), fewer light steps for
  far samples, quarter resolution with a better upsample.
- **Checks:** clean build at 127 warnings, module lint passes, the outside
  view is pixel-identical (a Jupiter close-up compared before/after). The
  bench was skipped at the user's request.
- **Where it stands:** a working volumetric cloud sea with domed,
  self-shadowed cumulus, warm shadows, sunlit tops toward the Sun and dark
  backlit masses from inside, fading smoothly in from orbit. Still soft
  (half resolution), and far from the painting's sculpted walls and
  shelves: that's stage 3.

### 3. Shaping toward the painting (medium)

- Shelf and anvil overhangs where bands shear: detail skewed with height
  along the wind shear.
- Storm walls around the vortices (the exterior storms, in 3D).
- Swept edges: detail noise stretched along the wind.
- Lightning in the water deck: brief flashes that light the cloud from
  inside, bluish.
- Light rays through gaps (the march's shadowing toward the Sun).

#### Stage 3 as built (2026-10-23, first pass)

- **Mesoscale structure in the weather map** (`gasweather.frag`): the
  exterior field hardly changes across the 2,000 km map (a few degrees of
  latitude), so the map was all belt or all zone and the clouds a uniform
  lumpy sea. The bake now adds cloud ranges `GAS_CLOUD_MASSIF_M` (150 km)
  across north-south, several times longer east-west (warped value noise
  in the storm-spun frame, so they wind round storms), with canyons of
  clear air between (about half the map). Ranges are built to full height
  right from their edge, so their sides are cliffs.
- **New map channels:** weather alpha is the signed wind shear (the band
  colour's gradient with latitude, where the jets run, plus a wandering
  local part); colour alpha is the storm walls (the collars round the
  spinning storms and polar cyclones, `GasCloud.wall`, a pure addition to
  `gasCloudField`).
- **Shaping** (`cloudshape.glsl`, the density now shared by the march and
  the shadow map): the shapes lean with the local shear (either way);
  where it shears, tops spread and flatten into anvils that overhang
  downwind and the trunk narrows (`CloudVolumeLayer.anvil`); thin flat
  shelves at two levels reach out from the clouds (`shelf`); the detail
  noise is stretched along the wind and squashed in height, so eroded
  edges trail as streaks; walls are full cover to the layer's top.
- **Taller decks:** middle layer -0.3 to 1.0 scale heights, top layer
  1.05 to 1.3. The middle one starts below the stand-in's cloud tops, so
  the walls rise out of the deck (flat bases above a bright deck read as a
  dark outline).
- **Lightning** (`CloudVolumeFlash`, up to 4): point lights in the lower
  middle deck, mostly ahead of the camera, 6-80 km off; three strokes over
  ~0.3 s, on the scene clock (real time; bench runs repeat). They light
  the cloud round them (falling off over 8 km, white-blue through the
  cloud's tint) and faintly the air. Sliders: Lightning (per s, default
  0.6), Flash brightness.
- **Shadow map / rays** (`cloudshadow.frag`, `cloudshadowmap.glsl`): each
  frame, 512² over 300 km round the camera, the clouds' optical depth
  toward the Sun from three heights in the shell (no detail noise). The
  haze pass and the march's air read it, so the Sun's light in the haze
  and murk is cut where clouds stand in its way: shafts through gaps. The
  clouds use it too, for clouds beyond their own short light march (x0.4:
  it has no erosion). In open canyons the shafts are subtle; they need the
  Sun behind broken cloud.
- **Fixes on the way:**
  - Dark rims along every cloud edge (also in stage 2): the temporal
    clamp clamped premultiplied colour and alpha one by one, pairing a
    thin edge's low colour with a thick neighbour's alpha. It now clamps
    alpha, and the colour per unit alpha, to the neighbourhood.
  - The air in front of the clouds is integrated along the march (each
    sample gets its own), not applied once at a weighted distance.
  - Inside thick cloud the scattering orders went to black; a diffusion
    term (light falling off as 1 / (1 + 0.1 tau), deep inside only) keeps
    a dim warm glow.
- **Cost (Iris Xe, 1280x720):** cloud volume ~34-36 ms; "Gas interior"
  (haze + shadow map + bake) ~13 ms. Room: shadow map every few frames or
  smaller, empty-space skipping in the march.
- **Where it stands:** cliffed ranges with billowing walls and canyons,
  lightning glowing inside them, interiors a dim warm fog. Open: the
  painting's creamy highlights against deep umber (contrast and colour
  tuning), shelves and anvils are subtle at most distances, and the camera
  often starts inside a range (the map is about half cloud).

### 4. Drifting layers (small)

- Each layer turns about the pole as a rigid shell at its own rate (from
  its wind speed), so neighbouring layers slide against each other. The
  rotation angle is computed in double from simulation time and wrapped,
  so it is exact at any time speed.
- The accumulation over frames reprojects each layer with its own motion.
- The top deck's rate can also turn the exterior bands, so outside and
  inside agree.

### 5. Hand-off and polish (medium)

- No visible jump between the exterior sphere and the interior as the
  camera descends.
- Quality presets, scratch scenes while iterating, and a bench scene once
  the look settles (that needs the user's go-ahead to add references).

## Open items

- Raising the GL context from 4.1 to 4.6 before stage 2 (macOS is no
  longer a target; see `docs/volumetric-clouds.md`, "OpenGL version").
  Compute shaders would make the weather bake, the noise volumes and the
  accumulation simpler and faster.

- The vertical exaggeration of the decks: start near the painting, tune
  by eye.
- At high time speeds the Sun crosses the sky in seconds inside a giant
  (a ~10 hour day). Later the app could drop to real time on entering a
  giant.
- Whether the rocky planets switch to the volume right after this, or
  stay on the slab for a while.

## Verification

- Build clean, no new warnings; `tools/check_modules.ps1`.
- While iterating: scratch `.bench` scenes descending into a giant, with
  `--shots`. No bench reference images change (the existing scenes never
  enter a giant, and bench time stays at 1440x).
- Perf: the interior passes in the profiler; A/B by alternating runs.
