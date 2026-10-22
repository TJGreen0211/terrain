# Step 5: a solar system (first chunk)

Status: 2026-10-21. The "real-ish compressed" design below was superseded the same day by a PROCEDURAL system; see "Procedural system (as built)" at the end. Kepler orbits, the focus camera and the stand-ins carried over.

The user's choices:
- **System:** "real-ish, compressed". Real bodies and orbital elements,
  with distances compressed so neighbours are visible and travel is
  quick.
- **Orbits:** Keplerian elements.
- **First chunk:** Sun-centred bodies, a camera attached to a focus body,
  and drawing of distant bodies.
- **Later:** per-planet terrain, atmosphere and ocean (the single-planet
  refactor), and new kinds of body (gas giant shading, Moon terrain).

## 5a. Sun-centred orbits (Bodies.c)

- **Body table:** Sun (the origin), Mercury, Venus, Earth, Moon (child
  of Earth), Mars, Jupiter, Saturn, Uranus and Neptune. Parents come
  before children.
- **Elements:** each body carries Keplerian elements at J2000 (simTime 0):
  a, e, i, Omega (longitude of the ascending node), varpi (longitude of
  perihelion) and L (mean longitude). Mean motion follows from the
  sidereal period. For the planets: JPL "approximate positions" table
  (Standish), with the Earth-Moon barycentre used for Earth. Moon,
  relative to Earth: a 384,400 km, e 0.0549, i 5.145 deg, period
  27.32 d. Node and perihelion precession are ignored.
- **Solving:** Kepler's equation is solved per frame in double with Newton
  iteration on M kept in [0, 2 pi), so long runs never lose precision.
  Elements are mapped from the ecliptic to the world: ecliptic north =
  world +Y, and orbits run counter-clockwise seen from +Y. That matches
  the existing prograde spin of Earth about its pole.
- **Compression:** `ORBIT_DISTANCE_SCALE` (WorldConfig.h, default 0.1)
  multiplies the semi-major axes of bodies orbiting the Sun. The Sun's
  radius is scaled by the same factor, so from Earth the Sun keeps its
  apparent size and direction, and the sky near Earth looks as before.
  Planet radii, the Moon's orbit and all periods stay real.
  - At 0.1, Earth is 1.5e10 m from the Sun.
  - Mars at closest approach is about 0.05 deg wide (a few pixels).
  - Jupiter at closest approach is about 0.13 deg wide.
- **Continuity with today's scene:** the ecliptic frame is rotated about
  +Y so that at simTime 0 the Sun is seen from Earth at
  `SUN_ORBIT_PHASE_DEG` (direction (-1, 0, -1)), as it is now. Earth's
  spin and tilt are unchanged, so t = 0 lighting is the same. Seasons and
  the difference between solar and sidereal days now emerge from the
  orbit.
- **Time scale:** `SIM_TIME_SCALE` becomes the default of a live time
  scale (debug panel slider, log-mapped), so outer orbits can be watched.

## 5b. Focus-body camera (arcballCamera.c)

- **Pivot:** stored relative to the focus body's position, as an offset in
  world axes that does not rotate with the body. The camera therefore
  rides along with the body's orbit. Main.c sets the focus origin each
  frame after `Bodies_Update`. `setCameraPivot(world)` keeps its meaning
  by converting to an offset, so Bench keeps working.
- **Switching:** Tab / Shift+Tab cycles the focus body, and the debug
  panel lists the bodies. A switch resets the pivot to the body's centre,
  backs off to 3 radii, and keeps the view rotation.
- **Surface preset (G):** stands at the focus body's real sub-solar point,
  using the Sun's actual direction, which now moves.
- **Limits:** `CAMERA_DISTANCE_MAX_M` grows to about the outer system's
  size. The orbit distance stays a float: only its rounding relative to
  the pivot matters.

## 5c. Distant bodies (new Planets.c, shaders/planet_impostor.*)

- **Shape:** each simple body (planets other than Earth) is drawn as a
  camera-facing quad around its silhouette cone, using exact ray-sphere
  intersection in the fragment shader.
  - It works in body radii relative to the body.
  - The miss distance comes from |o x d| and the normal from
    d x (o x d) - sqrt(1 - h^2) d. That avoids the cancellation that
    breaks the naive quadratic at 1e4+ radii.
- **Depth:** written as near / viewZ (reversed-Z), so bodies occlude and
  are occluded correctly, and the atmosphere's scene-depth clip sees
  them.
- **Shading:** Lambert from the Sun, with a per-body albedo colour.
  Stylised: gas giants get faint latitude bands, and Saturn gets a ring
  later.
- **Sub-pixel:** when the disc is smaller than about 1.5 px, the quad
  grows to a minimum size and becomes a soft dot. Its brightness keeps
  the disc's total light (albedo x Lambert phase integral x area), with a
  minimum brightness (stylised) so planets show like bright stars.
- **Earth and Moon when far:** once their full rendering would be below
  a few pixels, they switch to the same impostor. The planet, water,
  shadow, rings and atmosphere passes are skipped, which also saves time.
- **Sun:** unchanged. The disc and glow already scale with the Sun's
  radius and distance; they will be checked from the outer planets.

## Bench

- At t = 0 the Sun direction is unchanged. Over a 480-frame run Earth
  moves about 0.1 deg around the Sun, and the Moon's position differs
  from the old circular orbit.
- References may need updating, with the user's approval, if the Moon
  enters a shot.

## As built (2026-10-21)

- **Orbits (5a):**
  - The orbital elements are in WorldConfig.h as `<BODY>_ORBIT` lists.
    The solver and the turn of the ecliptic frame are in Bodies.c.
  - Checked with a standalone harness:
    - the Sun at (-1, 0, -1) from Earth at t = 0, 0.983 AU (perihelion);
    - after one sidereal year, back to the same spot;
    - Earth orbits in the same sense as its spin;
    - the Moon at 365k-400k km, up to 35,000 km off the ecliptic;
    - Mars between 1.381 and 1.666 AU from the Sun.
  - `g_simTimeScale` is live, with a log slider in the debug panel's
    "Solar system" section. Bench runs keep using `SIM_TIME_SCALE`.
- **Camera (5b):** the pivot is relative to `g_focusBody`.
  - `setCameraFocus` is called every frame from Main.c;
    `resetCameraForFocus` runs on a focus switch.
  - Tab / Shift+Tab, or the panel's body buttons, switch the focus.
  - Scrolling moves at least 10% of the current distance per notch.
  - The surface preset (G) uses the focus body's real sub-solar direction.
  - Bench scenes accept `pivot <body name>`, which orbits that body and
    focuses it.
- **Distant bodies (5c):** Planets.c and shaders/planet_impostor.*.
  - Earth and the Moon switch to the stand-in below 3 px radius. Their
    shadow, planet, water, ring and atmosphere passes are then skipped
    (shadows are skipped only when both are small).
  - Dots: Gaussian with sigma 0.7 px and a brightness floor of 3.0
    (look constants at the top of Planets.c).
- **Bench:** the sun-pivoted scenes (grazing-sea, topdown-detail) drift
  with Earth's orbit, about 20 px by frame 450, and need new references.
  The orbit-mid and whole-planet scenes are unchanged.

## Not done yet / ideas

- **Saturn's rings** on the stand-in. Earth's stylised rings (Rings.c)
  could be generalised.
- **The Sun's look from the outer planets:** the disc and glow scale with
  the compressed radius, and haven't been checked from far away.
- **Dots** read like coloured stars. A slightly larger sigma or a
  brighter floor would make planets stand out more.
- **The Moon's elements:** the ascending node's 18.6-year precession is
  ignored. Planet poles all lean about world X; real pole directions are
  not modelled.
- **Steps 4 and 5 still to do:** per-planet terrain and atmosphere (the
  single-planet refactor), and gas giant / Moon terrain kinds.

## Procedural system (as built, 2026-10-21)

The user's direction:
- **Made-up system:** every planet but Earth is generated from a system
  seed, separate from Earth's terrain seed.
- **Earth stays exactly as built:** 1 AU, its atmosphere, rings, oceans
  and Moon.
- **Names:** kept (Mercury ... Neptune), plus the dwarf planets Ceres,
  Pluto, Haumea and Makemake.
- **Gas giants:** random, but weighted outward. They keep the banded
  stand-in; only their rings are procedural for now.
- **Rocky planets:** get the full treatment (terrain, atmosphere, rings),
  baked on approach and cached until a restart or reseed.
- **Water:** disabled on every planet but Earth until the water rework.

**Generator** (SolarSystem.c, ranges in WorldConfig.h "Procedural system"):
- **Orbit slots, outward:** Mercury, Venus, [Earth], Mars, Jupiter,
  Saturn, Uranus, Neptune. Each is spaced from the previous one by a random
  ratio of 1.4-1.9, outward and inward from Earth's 1 AU.
- **Dwarf orbits:** Ceres sits between the 4th and 5th slots. Pluto,
  Haumea and Makemake sit 1.25-1.7x beyond the last planet, with
  eccentricity up to 0.3 and inclination up to 25 deg.
- **Gas giant chance:** 3% at 1 AU or less, rising to 90% at 5 AU or more
  (log-linear in distance).
- **Radii:** rocky 0.3-1.5 Earth radii; dwarfs 300-1200 km; gas giants
  22,000-75,000 km.
- **Rocky planets and dwarfs:**
  - **Atmosphere:** the chance grows with size. Each atmosphere gets a
    random colour (C_R), a shell 0.5-1.4x Earth's relative to the radius,
    and random K_R and K_M.
  - **Water:** `hasWater` is generated (atmosphere plus a temperate
    orbit) but not drawn; `OTHER_PLANET_WATER` is 0.
  - **Palette:** one base hue, 70% greys, ochres and rusts, otherwise
    faint. Temperature comes from distance via `tempOffset`. Humidity is 0.
  - **Terrain:** the bake settings are Earth's, varied (plates, continent
    area, relief, fewer droplets).
- **Rings:** gas giants 50%, rocky 15%, dwarfs 8%. Equatorial, with their
  own profile seed. Earth keeps its fixed stylised ring plane.

**Active planet** (one terrain planet in full at a time):
- **The globals are the active planet:** `g_planetGen`, `g_planetStyle`,
  `g_atmosphere`, `g_terrainBake`, `planetHeightTex` and the ring state.
  `SolarSystem_Activate` saves the previous planet into its `PlanetInfo`
  and loads the next. The debug panel, H key and regenerate button
  therefore act on the active planet.
- **Choosing it:** `SolarSystem_PickActive` picks the terrain planet
  largest on screen once it is at least 2 px, with 1.25x hysteresis.
- **First bake, in the background** (`SolarSystem_Update`, each frame):
  - When PickActive wants an unbaked planet, its bake starts and the
    planet stays a stand-in. The switch happens once the bake is done.
  - The focused planet (Tab) is pre-baked whenever the slot is free.
  - One bake at a time. A reseed drops a bake in flight when it finishes.
  - The debug panel shows "Baking <name>... N%".
- **How the background bake avoids stalls** (`TerrainGen_BakeStart/Poll/
  Finish/Cancel`):
  - The GPU shaping pass is drawn in 32 scissored strips, one per frame.
  - The readback is GL_RG into a pixel buffer, collected once a fence
    signals (about 6 ms copy).
  - Erosion, the climate and ocean maps and the range pyramid run on a
    below-normal-priority worker thread.
  - The three textures are uploaded one per frame (each 10 ms or less).
  - Results: worst frame about 11 ms during a Venus bake.
    Background-baked and blocking-baked Venus render pixel-identical.
  - The bake takes about 4-8 s in the background versus about 2.5 s
    blocking: the laptop shares its power budget between the cores.
- **Blocking bakes** remain for start-up, H / regenerate and bench runs.
  A blocking bake first completes any background job (the shaping
  target and the CPU stage's scratch state are shared). Bench scenes
  can opt into background baking with `bakeasync` (not repeatable).
- **TerrainGen:** each bake owns its range table (`PlanetGenParams.pyramid`)
  and its climate and ocean maps (`TerrainGen_FreePlanet`); the range
  queries read the active planet's.
- **Dry worlds:** sea level drops to the lowest point (all land).
  quad.frag now scales all moisture by `humidity` and suppresses lakes and
  salt pans at humidity 0 (Earth, at humidity 1, is unchanged).
- **Earth-only parts:** water pass, FFT waves, the Moon as a caster and
  receiver.
- **Rings:** `Rings_SetPlanet(enabled, seed, normal)`.

**Stand-ins** (Planets.c, planet_impostor.frag):
- body-fixed mottling on rocky planets;
- an atmosphere rim and glow in the planet's sky colour;
- a ring sheet with seeded bands that shadows the planet and is shadowed
  by it.

**Verified:**
- Earth's visual check is unchanged; only the earlier orbit drift
  remains.
- Earth -> Venus -> Earth gives pixel-identical Earth frames.
- Repeat visits don't re-bake.
- Day-side renders of Venus (atmosphere), Mercury (rings) and Ceres;
  ringed gas giant stand-ins.

**Bench additions:**
- `pivot <body>`, `pivot subsolar <body>`;
- `pivotat <frame> <body>`;
- `systemseed N`;
- `bakeasync` (background bakes, as in normal use);
- `poff ground`, `timescale`.

**Known / next:**
- Async bake: done (see above). The H / regenerate re-bake still blocks.
- The switch from stand-in to full detail pops in colour and brightness.
  The stand-in's flat colour and rim don't match the baked surface and
  the scattering.
- Plateau snow can show hard edges from the plateau shapes in the
  heightmap.
- The stand-ins have no surface detail from the bake (they could sample
  the baked maps once baked).
- Gas giant generation and look are still to come.
- Ring rock and dust sizes are in metres, sized for Earth.
- Moons for other planets.
