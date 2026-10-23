# Gas giants

Status: 2026-10-22. Phase 1 built and polished (see "Phase 1 as built" and
"Phase 1 polish"). Phase 2 (the flow bake) was built, judged worse than
Phase 1 by the user (marbled, liquid, too busy) and removed; see "Phase 2:
tried and dropped". The direction is to keep Phase 1's smooth, wispy look
and build on it. The exterior comes first; the interior (flying inside the
cloud decks) comes later and builds on it.

The user's choices:
- **Look:** Jupiter and Neptune from outside, leaning toward a warm,
  painterly style rather than strict realism. The target mood (for the
  interior later) is a concept painting: creamy, high-key haze, soft
  glowing light, rust and umber cloud masses with sculpted, swept edges.
- **Detail:** smooth, wispy and cloud-like. Not marbled, liquid or busy.
- **Motion:** a static look for now. Animation is deferred (see "Later").
- **Bench:** no gas-giant bench scene while the look is being iterated.

## Before this work

- Gas giants were stand-ins only (`planets.c`, `planet_impostor.frag`): an
  exact ray-traced sphere, which stays as the base.
- Their whole look was one shader branch: `sin(lat * 22 + ...)` bands
  scaling a single albedo colour, a linear limb darkening and Lambert
  light.
- Generation (`solar_system.c`, `SolarSystem_Generate`) picked a radius, a
  spin, one hue and a ring chance. `PlanetInfo` was zeroed, so
  `atmo.shellHeight` was 0: no limb glow.
- Rings, ring shadows on the sphere and the sphere's shadow on the rings
  already worked and are kept as they are.

## What the look needs

1. **Irregular zonal structure:** bands of uneven widths, each with its
   own colour, and edges that range from sharp to soft. Jupiter-like
   planets have many bands; Neptune-like ones a few broad, soft ones.
2. **Turbulence:** curls where the bands shear, streamers, and oval
   vortices (red spots, white ovals, dark spots), kept soft and streaky.
3. **Lighting through a deep atmosphere:** strong limb darkening, a soft
   terminator, a high haze layer and a bright rim at the limb.
4. **High clouds:** bright cirrus or plumes above the main deck.

### The painterly lean

- **Palettes:** generated ramps biased warm. Cream and ivory zones, ochre,
  rust and umber belts, and warm grey-brown poles. Ice giants stay blue
  but desaturated and lifted toward teal-grey, never pure saturated blue.
  A small chance of exotic palettes, kept harmonious (adjacent hues on one
  ramp, not random colours per band).
- **High key and soft:** zones lift toward the light colour, the haze
  adds a warm veil that grows toward the limb, and the night side stays
  dark behind a warm terminator.
- **Sculpted edges:** the cloud texture's slope toward the Sun shades the
  cloud masses, so they read as rounded and swept, not flat paint.
- **Glow:** a broad forward-scatter rim on the lit limb and a thin
  crescent glow when backlit.

These are look constants, not scale constants: they live at the top of the
module's files, as in `planets.c`. Anything scale-dependent goes in
`WorldConfig.h`.

## Module

- `src/world/gasgiant/` (`GasGiant_` prefix, `gas_giant.c/.h`,
  `module.mk`, `shaders/gasgiant.glsl`, a `GasGiant_DebugPanel` tab
  registered in `app.c`).
- `planet_impostor.frag` includes `gasgiant.glsl` and calls
  `gasGiantSurface` / `gasGiantGlow` when `gasOn`; the ray-sphere, depth,
  ring and dot code stays shared.
- `PlanetInfo.gas` holds each giant's `GasGiantStyle` and its profile
  texture.

## Phases

### Phase 1: style and shading

- **Style:** band count, jittered band edges and widths, a palette ramp, a
  turbulence amount, storms and a haze, and a family (Jupiter-like,
  Saturn-like, ice giant, exotic).
- **Latitude profile:** a small 1D texture built on the CPU from the
  style.
- **Shader:** band lookup by latitude, warped by noise; limb darkening;
  wrap lighting at the terminator; the warm haze veil; the limb rim and a
  glow beyond the limb.

### Phase 1 as built (2026-10-22, committed as "Started work on gas giant rendering")

- **Module:** `src/world/gasgiant/` as above. `Body.banded` and the old
  sine bands are gone.
- **Style:** `PlanetInfo.gas` (`GasGiantStyle`), generated in
  `SolarSystem_Generate` from its own RNG stream (4000 + id), so every
  other generated property is unchanged.
  - Families: Jovian, Saturnian, ice giant and exotic. Ice giants are
    likelier farther out (`GAS_ICE_*` in WorldConfig.h).
  - Storms: Jovian giants get a big red spot plus white ovals; ice giants
    get dark spots with a bright companion cloud.
- **Profile:** 1024-texel RGBA16F 1D texture (RGB band colour as display
  values, A turbulence weight).
  - It is rebuilt when an FNV hash of the style changes (debug panel
    edits).
  - The dot colour (`Body.color`) is the profile's area-weighted mean.
- **Noise:** a shared 128³ RGBA8 texture of four periodic value noises (32
  lattice cells per tile, built at startup by `GasGiant_Init`).
  - Each FBM octave is one trilinear fetch giving four noises.
  - Octaves finer than a pixel fade to their mean.
  - Hashed ALU noise had cost 13 ms at full screen.
- **Shading:**
  - Storms spin the lookup latitude/longitude into a spiral and add a pale
    collar.
  - A two-level domain warp curls the band edges. It is stretched
    east-west and scaled by the profile's turbulence.
  - The cloud texture modulates brightness, and its slope toward the Sun
    gives the relief.
  - Light is Minnaert (k) on wrap-lit cloud tops, reddened toward the
    terminator, under a haze veil with a Henyey-Greenstein rim. The shell
    beyond the limb glows with the same light.
  - Everything is lit in linear and tone mapped back, so straight-down
    full sun returns the profile colour.
- **Debug panel:** "Gas giants" tab:
  - Buttons to focus each giant.
  - The focused giant's style: family, seed / re-roll, bands, palette
    pickers, turbulence, storms, haze.
  - The shared look: limb k, wrap, terminator tint, night ambient, haze
    rim, limb glow, exposure.
- **Profiler:** a "Stand-ins" section around `Planets_Draw`.

### Phase 2: tried and dropped (2026-10-22)

- **What it was:** a GPU flow bake. It used back-traced characteristics,
  not step-by-step advection: each cube texel traced its parcel back
  through jets, curl-noise eddies and storm vortices, and summed the bands
  it passed. That is exact advection with relaxation, with no resampling
  blur. It baked a 512² cube per giant over a few frames.
- **Result:** physically more fluid, but it read as a marbled, liquid,
  overly busy texture. The user preferred Phase 1's smooth, wispy,
  cloud-like look, so the code was removed (it was never committed).
- **Lessons for later work:**
  - Keep detail soft and streaky.
  - Avoid isotropic noise ridges and strong isotropic warps (they read as
    marbling or caustics).
  - Show renders before replacing the look.
  - If a bake is ever needed (e.g. for the interior's weather map), bake
    the Phase 1 function itself rather than a flow simulation.

### Phase 1 polish (2026-10-22)

- **Wisps** (`GasGiantStyle.wisps`, panel "Wisps"):
  - Bright cirrus-like strands. They are ridges of a noise stretched
    strongly east-west in the warped space, so they are long strands that
    bend with the streamers.
  - A first try used ridges of the plain warped noise; it made a
    caustic-like cell network and was dropped.
  - They fade out before they would alias, and also up close once a strand
    would be more than a few pixels wide (broad strands look like satin).
  - Defaults: Jovian 0.7, ice 0.6, exotic 0.55, Saturnian 0.45.
  - They cost about 0.7 ms of a ~6.5 ms full-screen 1080p close-up
    (alternating A/B runs).
- **Polar cyclones** (`polarCount`, `polarLat`, `polarSize`, `polarSpin`,
  `polarColor`; panel group "Polar cyclones"):
  - A ring of cyclones around each pole plus one on it, as Juno saw on
    Jupiter. They are worked out in the pole's azimuthal-equidistant plane
    and spin the lookup like the storms do.
  - Each has a darker, cooler core and a pale collar. Angle, radius and size
    are jittered so the ring isn't a perfect polygon.
  - Per family: Jovian 5-8 in the ring; Saturnian only the pole's; ice
    giants 40% only the pole's, otherwise none; exotic random.
  - They have their own RNG stream (5).
- **Polar caps:**
  - The cap fades in over about 15° of latitude instead of having a hard
    edge.
  - The cap edge is no longer a turbulence peak; that peak made a bright,
    curly, crater-like rim.
  - The cap's turbulence weight dropped 0.8 → 0.4, since strong warps
    there read as marbling.
- **Storms:** up to 10 (was 6). Jovian giants add:
  - 0-2 "barges": long dark ovals in the belts, turning slowly.
  - Half the time, a "string of pearls": 4 small white ovals in a row
    near 35-42° S.
- **Calmer non-Jovian families** (the user found Saturn and Uranus too
  busy; Neptune was fine): fewer bands (Saturnian 6-10, ice 3-6, exotic
  6-12), weaker fine streaks within bands, and wisps down to 0.25-0.3.
  Ice giants also stretch their cloud texture less (2.5-4, it made
  pinstripes) with lower texture contrast. Only the ranges changed, not the
  number of random draws, so every other generated property is the same.
  Jovian giants are unchanged.
- **Close-up octaves:** tried 6-7 octaves for the detail. They added
  almost nothing visible at 1080p and cost about 1 ms, so they were
  reverted to 5.
- **Cost:** the full-screen 1080p close-up runs at ~6.5 ms GPU for the
  stand-in pass on the Iris Xe in the same thermal state. The earlier
  4.7 ms was a cooler-laptop reading.

### Phase 3: close-up detail (in progress)

- **High clouds** (built 2026-10-22; `GasGiantStyle.high`, `highHeight`,
  `highColor`, `highCount`, `highBand[]`; panel group "High clouds"):
  - A sparse layer of bright streaks and plumes above the deck
    (`gasHighCover` in `gasgiant.glsl`). It is a thresholded noise,
    stretched east-west (1.5 times the deck's stretch, at 2.5 times its
    frequency), with a gentle north-south wobble and no isotropic warp.
    It uses the storm-spun position, so it curls around storms.
  - It gathers in up to 4 latitude bands (Gaussian in latitude, each with
    a strength). Outside the bands the noise isn't evaluated, so the cost
    is only paid where the clouds are.
  - Shading: above most of the haze (40% of the deck's veil), milder limb
    darkening (Minnaert k halfway to 1), the deck's cloud texture for some
    variation, and dimmer where more cloud lies sunward (its shaded side).
  - Shadows: the cover is sampled again at the point offset toward the Sun
    by `highHeight * tan(sun zenith)` (capped at 0.08 radii) and darkens
    the deck's direct light by up to 60%. Shadows lengthen toward the
    terminator.
  - Per family (own RNG stream, 6): Jovian rows of equatorial plumes at
    ±5-9° plus one mid-latitude band; ice giants Neptune-like streaks in
    2-3 mid-latitude bands, highest (0.006-0.01 radii) and strongest;
    Saturnian one faint band; exotic 1-3 random bands.
  - A first try stretched the noise 2.5 times further than the deck at a
    lower frequency: one noise cell spanned the planet, so the band came
    out as a solid white ring.
  - Not done: view parallax (the layer is drawn at the deck point; at
    these heights the shift is under a pixel except right at the limb).
- Close-ups are smooth and silky; finer deck detail is still open. Keep
  it soft and streaky (see the Phase 2 lessons).

### Phase 4: extras (optional)

- A Saturn-style polar hexagon.
- Night side: lightning flashes and aurora.
- Moon shadow transits, once procedural moons exist
  (`docs/procedural-moons.md`).

## Later

- **Animation:** differential rotation by offsetting longitude by
  `u(lat) * t` at lookup, with two-phase blending so the pattern does not
  smear; storms drift with their latitude's wind.
- **Interior:** the camera inside the shell, flying through volumetric
  cloud decks. Planned in `docs/gas-giant-interior.md`.

## Verification

- No bench scene or reference image shows a gas giant larger than a dot,
  so the existing checks only catch regressions (and dots must stay
  unchanged).
- While the look is being iterated, renders come from scratch `.bench`
  scenes (`pivot <body>`; yaw keys less than 180° apart, since the parser
  takes the shortest path; `systemseed N` to pick other systems) run with
  `--shots`. Seed 1 has Jovian, exotic and ice giants; seed 7's Jupiter
  is a ringed Saturnian.
- Perf to watch: the stand-in pass when a giant fills the screen (A/B by
  alternating runs on this throttling laptop).
