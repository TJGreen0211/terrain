#ifndef WORLDCONFIG_H
#define WORLDCONFIG_H

/* ======================================================================
   WORLD CONFIGURATION ??? every world-scale constant lives here.

   Units: 1 world unit = 1 metre. Angles in degrees unless the name says
   _RAD. Times in seconds of simulation time.

   Precision rule: positions, orbits, rotations and time are double on the
   CPU. The GPU only ever sees camera-relative (or body-local) floats; see
   bodies.c and camRelTranslate in main.c.

   Shading / look constants that are already expressed as fractions of a
   body radius (atmosphere shell, terrain relief, ring radii, ...) stay with
   the code that uses them, since they do not change with scale.
   ====================================================================== */

/* ---- Physical constants ------------------------------------------------ */

#define AU_M 1.495978707e11

/* ---- Bodies ------------------------------------------------------------
   The Sun is the world origin; every other body follows a Kepler ellipse
   around its parent (bodies.c). Earth (the Earth-like planet we build on)
   and its Moon are fixed here; every other planet is generated from the
   system seed (solar_system.c, "Procedural system" below).

   _ORBIT lists: a (m), e, i, node, peri, L, period (s)
     a       semi-major axis (before ORBIT_DISTANCE_SCALE)
     e       eccentricity
     i       inclination to the ecliptic (deg)
     node    longitude of the ascending node (deg)
     peri    longitude of perihelion (deg) = node + argument of perihelion
     L       mean longitude at t = 0 (deg)
     period  sidereal orbital period; 0 = from a by Kepler's third law
   The ecliptic maps to the world XZ plane with its north at world +Y;
   orbits run counter-clockwise seen from +Y.

   Rotation (body -> world) = rotateX(AXIAL_TILT - 90) * rotateZ(spin): the
   body's local +Z pole is tipped up to world +Y, then leaned about world X
   by the tilt (> 90 = retrograde spin). spin = 360 * t / ROTATION_PERIOD. */

/* Compression (look, not physics): the semi-major axes of bodies orbiting
   the Sun, and the Sun's radius, are multiplied by this, so neighbours are
   visible and quick to reach while the Sun keeps its apparent size seen
   from each planet. Planet radii, the Moon's orbit and all periods are
   unscaled. 1 = true scale. */
#define ORBIT_DISTANCE_SCALE      0.1
/* Where the Sun appears from Earth at t = 0 (world XZ angle; 225 = toward
   (-1, 0, -1)): the ecliptic is turned about +Y to put it there. */
#define SUN_ORBIT_PHASE_DEG       225.0
/* Sidereal year: periods from Kepler's third law, P = year * (a/AU)^1.5. */
#define SIDEREAL_YEAR_S           31558149.8

#define SUN_RADIUS_M              6.957e8    /* drawn x ORBIT_DISTANCE_SCALE */

#define EARTH_RADIUS_M            6.371e6
#define EARTH_ORBIT               1.0*AU_M, 0.0167, 0.0, 0.0, 102.94, 100.46, 0.0
#define EARTH_ROTATION_PERIOD_S   86164.0905 /* sidereal day */
#define EARTH_AXIAL_TILT_DEG      23.44

#define MOON_RADIUS_M             1.7374e6
/* Relative to Earth; not compressed. Period: 27.32 d sidereal. */
#define MOON_ORBIT                3.844e8, 0.0549, 5.145, 125.045, 83.353, 218.316, 2360591.5
#define MOON_AXIAL_TILT_DEG       1.54
/* Tidally locked: one spin per orbit. */
#define MOON_ROTATION_PERIOD_S    2360591.5

/* ---- Procedural system (solar_system.c) ----------------------------------
   Generated from the system seed (debug panel; separate from Earth's
   terrain seed). Planets keep their names and order outward from the Sun:
   Mercury, Venus, [Earth], Mars, Ceres, Jupiter, Saturn, Uranus, Neptune,
   Pluto, Haumea, Makemake. Major planets are spaced by a random ratio
   from Earth's 1 AU, inward and outward; Ceres sits between the 4th and
   5th planets (an asteroid-belt slot), and Pluto, Haumea and Makemake
   beyond the last major planet, on eccentric, inclined orbits. */
#define SYSTEM_SEED_DEFAULT       1u
#define SYSTEM_SPACING_MIN        1.4        /* orbit ratio between neighbours */
#define SYSTEM_SPACING_MAX        1.9
/* A major planet is a gas giant with a chance rising from GAS_P_INNER at or
   inside GAS_AU_INNER to GAS_P_OUTER at or beyond GAS_AU_OUTER (log a). */
#define GAS_P_INNER               0.03
#define GAS_P_OUTER               0.9
#define GAS_AU_INNER              1.0
#define GAS_AU_OUTER              5.0
#define ROCKY_RADIUS_MIN_M        (0.3 * EARTH_RADIUS_M)
#define ROCKY_RADIUS_MAX_M        (1.5 * EARTH_RADIUS_M)
#define DWARF_RADIUS_MIN_M        3.0e5
#define DWARF_RADIUS_MAX_M        1.2e6
#define GAS_RADIUS_MIN_M          2.2e7
#define GAS_RADIUS_MAX_M          7.5e7
/* Gas giant looks (world/gasgiant): the chance of an ice giant rises (log a)
   from 0 at GAS_ICE_AU_INNER to GAS_ICE_P_OUTER at GAS_ICE_AU_OUTER. The
   haze shell (the glow beyond the limb) is a fraction of the radius. */
#define GAS_ICE_P_OUTER           0.6
#define GAS_ICE_AU_INNER          3.0
#define GAS_ICE_AU_OUTER          15.0
#define GAS_SHELL_FRACTION_MIN    0.012
#define GAS_SHELL_FRACTION_MAX    0.03
/* Lightning seen from outside (gas_giant.c): the glow a flash spreads
   through the cloud over (radius, m; the debug panel's starting value).
   Real ones seen from orbit spread ~100-500 km; larger here, so they read
   as lit storm cells from orbit, not as stars. */
#define GAS_FLASH_SIZE_M          800.0e3
/* The aurora's curtains (gasgiant.glsl): their tops above the cloud tops,
   in radii (Jupiter's real ones reach ~0.005; taller here, so they stand
   off the limb from orbit). The stand-in quad grows to fit them. */
#define GAS_AURORA_HEIGHT_RADII   0.02
/* Inside a gas giant (world/gasgiant/gas_interior.c, docs/gas-giant-
   interior.md). Altitudes are above the cloud tops (the stand-in sphere).
   The haze over them falls off exponentially; the glow shell
   (shellFraction) spans GAS_HAZE_TOP_H of its scale heights, never less
   than GAS_HAZE_SCALE_HEIGHT_MIN_M (Jupiter's real one is ~27 km). The
   interior pass takes the haze over from the stand-in's veil between
   GAS_INTERIOR_FADE_LO_H and _HI_H scale heights. Below the cloud tops a
   murk (extinction per metre) darkens down to the floor the camera stops
   at, GAS_INTERIOR_FLOOR_M below them (under the water clouds). Within
   GAS_CLOSE_DETAIL_ALT_RADII of the cloud tops the stand-in adds detail
   octaves (they cost time and show nothing from farther out). */
#define GAS_HAZE_TOP_H            12.0
#define GAS_HAZE_SCALE_HEIGHT_MIN_M 10.0e3
#define GAS_CLOSE_DETAIL_ALT_RADII  0.05
/* The volumetric cloud layers (world/clouds/cloud_volume.c), in haze
   scale heights above the cloud tops (the stand-in's sphere): a tall
   middle layer (rust) and a thin top one (bright). Taller than Jupiter's
   real decks (two scale heights, ~55 km, for the middle one), for the
   painting's towering walls. Under them a flat, opaque cloud deck at
   GAS_CLOUD_DECK_H, drawn to the horizon, with a mist rising off it
   (scale height GAS_CLOUD_DECK_MIST_H, optical depth GAS_CLOUD_DECK_MIST_TAU
   straight up from the deck): the middle layer starts below it,
   so the walls' feet sink into it.
   They cross-fade in (their opacity, not their density) below
   GAS_CLOUD_FADE_HI_H scale heights and are fully there below _LO_H. Their
   weather map is a patch under the camera, GAS_CLOUD_MAP_EXTENT_M wide
   among the clouds, from higher up GAS_CLOUD_MAP_HORIZONS times the
   distance to the horizon (at most GAS_CLOUD_MAP_MAX_RADII radii); they
   reach GAS_CLOUD_REACH_M, from higher up the far cloud tops at the horizon
   (within 0.45 of the map either way), or as far as the haze lets anything
   be seen, fading out by opacity from 35% of that. */
#define GAS_CLOUD_MID_BASE_H      0.0
#define GAS_CLOUD_MID_TOP_H       2.0
#define GAS_CLOUD_TOP_BASE_H      2.1
#define GAS_CLOUD_TOP_TOP_H       2.5
#define GAS_CLOUD_DECK_H          0.35
#define GAS_CLOUD_DECK_MIST_H     0.05
#define GAS_CLOUD_DECK_MIST_TAU   1.0
#define GAS_CLOUD_REACH_M         2.6e6
#define GAS_CLOUD_FADE_LO_H       10.0
#define GAS_CLOUD_FADE_HI_H       45.0
#define GAS_CLOUD_MAP_EXTENT_M    6.0e6
#define GAS_CLOUD_MAP_HORIZONS    2.4
#define GAS_CLOUD_MAP_MAX_RADII   1.2
/* The clouds' shapes are placed in a frame that stays put while the map
   re-centres; it moves (the shapes jump) only once the camera is this far
   from its centre. */
#define GAS_CLOUD_NOISE_FRAME_M   5.0e6
/* The cloud ranges in that map: their size north-south (several times
   longer east-west), with canyons of clear air between them. */
#define GAS_CLOUD_MASSIF_M        150.0e3
/* How far the storm cells' anvil plumes stream downwind (the cloud
   volume's plume pass), fading out over that length. */
#define GAS_CLOUD_PLUME_M         500.0e3
#define GAS_INTERIOR_FADE_LO_H    4.0
#define GAS_INTERIOR_FADE_HI_H    8.0
#define GAS_DECK_MURK_PER_M       5.0e-5
#define GAS_INTERIOR_FLOOR_M      120.0e3
/* Flight near a gas giant: the camera's speed scales with altitude above
   the cloud tops, full speed at GAS_FLIGHT_FULL_SPEED_ALT_M and above,
   never below GAS_FLIGHT_MIN_SPEED_SCALE of it. */
#define GAS_FLIGHT_FULL_SPEED_ALT_M  2.0e7
#define GAS_FLIGHT_MIN_SPEED_SCALE   2.0e-4
/* Flight near a terrain planet (Earth, rocky, dwarf): the same, with the
   altitude above the highest ground within about that altitude around the
   nadir (sea level for planets not drawn in full). Full speed from
   TERRAIN_FLIGHT_FULL_SPEED_ALT_RADII of the planet's radius up (or that
   deep below the ground); the minimum is ~40 m/s at the default slider
   speed, so the camera never stalls at the surface. */
#define TERRAIN_FLIGHT_FULL_SPEED_ALT_RADII 1.0
#define TERRAIN_FLIGHT_MIN_SPEED_SCALE      1.0e-5
#define TERRAIN_FLIGHT_GROUND_CAP_M         100.0
/* Rocky planets: atmosphere shell as a fraction of the radius (Earth's look
   is ATMOSPHERE_SHELL_HEIGHT_M / EARTH_RADIUS_M), times a random factor. */
#define ROCKY_SHELL_FRACTION      (ATMOSPHERE_SHELL_HEIGHT_M / EARTH_RADIUS_M)
/* Habitable zone (AU): a planet with air orbiting inside it may have
   water. Stylised, far wider than a real one, so the inner and outer
   neighbours (Venus to Mars slots and a bit beyond) can be wet worlds. */
#define HABITABLE_AU_MIN          0.5
#define HABITABLE_AU_MAX          2.5
/* Water on the other planets generated with hasWater (sea level, water
   shell and waves, like Earth's). 0 = every planet but Earth is dry. */
#define OTHER_PLANET_WATER        1

/* ---- Time --------------------------------------------------------------
   Simulation seconds per real second (R pauses). The app starts in real
   time (the gas giants' cloud layers drift at their true speed); the debug
   panel's time-scale slider speeds it up. */
#define SIM_TIME_SCALE            1.0
/* Bench runs: 1440 = one Earth day per real minute (the old default, so
   the bench scenes' lighting and motion are unchanged). */
#define BENCH_TIME_SCALE          1440.0
#define SIM_TIME_SCALE_MAX        3.0e7      /* ~1 year per real second */

/* ---- Camera (metres) ---------------------------------------------------- */

/* Main camera near plane. The main pass is infinite reversed-Z into a float
   depth buffer, so this can sit at surface scale. */
#define CAMERA_NEAR_M             0.1
/* Vertical field of view (degrees). The terrain LOD split rule uses it too. */
#define CAMERA_FOV_Y_DEG          45.0
/* Orbit pivot spawn, on +Z from the planet centre. */
#define CAMERA_SPAWN_DISTANCE_M   (3.0 * EARTH_RADIUS_M)
/* Scroll-out limit of the orbit radius behind the pivot. */
#define CAMERA_DISTANCE_MAX_M     2.0e12   /* the whole generated system at ORBIT_DISTANCE_SCALE 0.1 */
/* Orbit distance after Tab switches the focus body, in its radii. */
#define CAMERA_FOCUS_DISTANCE_RADII 3.0
/* WASD speed (m/s) and scroll step (m per wheel notch): defaults, and the
   log-mapped ranges of the two on-screen sliders. The minimums reach
   walking pace so the surface can be inspected up close. */
#define CAMERA_MOVE_SPEED_MPS     (0.667 * EARTH_RADIUS_M)
#define CAMERA_MOVE_SPEED_MIN     1.0
#define CAMERA_MOVE_SPEED_MAX     (6.667 * EARTH_RADIUS_M)
#define CAMERA_SCROLL_STEP_M      (0.0333 * EARTH_RADIUS_M)
#define CAMERA_SCROLL_STEP_MIN    0.1
#define CAMERA_SCROLL_STEP_MAX    (0.667 * EARTH_RADIUS_M)
/* Sub-solar surface preset (G): altitude range measured from the planet
   centre, and the view tilt below the horizon. */
#define SURFACE_CAMERA_MIN_RADIUS_M (1.1 * EARTH_RADIUS_M)
#define SURFACE_CAMERA_MAX_RADIUS_M (1.483 * EARTH_RADIUS_M)
#define SURFACE_CAMERA_TILT_DEG     24.0

/* ---- Terrain LOD (quadtree) ---------------------------------------------
   A node splits when the camera is closer than K node widths to it (its
   height range included). K comes from the target on-screen triangle size:
   at the split distance a triangle edge spans QT_TARGET_TRIANGLE_PX, and
   just after the split half that. Smaller = finer and more patches (the
   count grows with 1/px^2). */
#define QT_TARGET_TRIANGLE_PX     24.0
/* The shadow-caster walk's target: casters only need to resolve the shadow
   map, not the screen, and nothing culls them (the sun's box holds the
   whole planet), so they refine much more coarsely. */
#define QT_SHADOW_TRIANGLE_PX     96.0
/* The water shell's target: half the terrain's LOD. A sphere at sea level
   barely needs vertices for its shape (the corner path is exact), only for
   the swell and smooth shading. */
#define QT_WATER_TRIANGLE_PX      (2.0 * QT_TARGET_TRIANGLE_PX)
/* Deepest level: subdivide until grid vertices are this far apart (m).
   ~20 levels for Earth. The shadow-caster walk stops earlier, at half a
   shadow-map texel. */
#define QT_MIN_VERTEX_SPACING_M   1.0
/* Patch-edge skirts (walls hiding LOD seams). A seam can only open as far
   as heights differ along the edge, so each patch's wall drops
   RANGE_FACTOR x its own height range + WIDTH_FRACTION x its width: tiny
   on flat ground and water, deep in mountains. Every wall fragment runs the
   full surface shader, so deeper than needed is expensive overdraw. Capped
   at 0.5% of the radius. */
#define QT_SKIRT_RANGE_FACTOR     1.5
#define QT_SKIRT_WIDTH_FRACTION   0.02

/* ---- Relief (PlanetStyle.landRelief / oceanRelief, terrain_gen.h) ---------
   About twice real Earth (docs/terrain-relief.md): land up to ~18 km, the
   seafloor down to ~22 km along the shelf profile. Relief is a ground-scale
   quantity, chosen in metres whatever the planet's radius (the terrain keeps
   a human-scale look on a shrunken planet too; the style stores it as a
   fraction of the radius, converted once). Generated planets draw theirs
   from the metre ranges below (solar_system.c). Edited live in the debug
   panel's Surface section (a re-bake updates the occlusion). */
#define EARTH_LAND_RELIEF_M       17700.0
#define EARTH_OCEAN_RELIEF_M      22000.0
#define ROCKY_LAND_RELIEF_MIN_M   8000.0
#define ROCKY_LAND_RELIEF_MAX_M   30000.0
#define DWARF_LAND_RELIEF_MIN_M   6000.0
#define DWARF_LAND_RELIEF_MAX_M   20000.0

/* Ground-scale shaping (heightbake.frag via upload.c), in metres whatever
   the planet's radius (docs/terrain-relief.md section 0): the mountain
   belts' widths (Gaussian half-widths about the plate boundary; the
   coastal range sits one width inland), the high plateau behind a
   collision, and the starting wavelengths of the noise in the ranges, the
   old worn ranges, hill country and rock texture (each runs down to two
   map texels). TERRAIN_MOUNT_GAIN: the ranges' ridges' amplitude per
   octave (0.5 = smooth classic sum; higher = more ridges and valleys at
   tens of km). TERRAIN_HYPSO_GAMMA: the land's heights (fraction of the
   relief) are raised to this power after the bake, so most land stays low
   and the peaks keep their height (Earth's hypsometric curve). */
#define TERRAIN_COLLIDE_WIDTH_M         250000.0
#define TERRAIN_ANDES_WIDTH_M           180000.0
#define TERRAIN_TIBET_WIDTH_M           900000.0
#define TERRAIN_MOUNT_WAVELENGTH_M      500000.0
#define TERRAIN_MOUNT_GAIN              0.8
#define TERRAIN_OLD_RANGE_WAVELENGTH_M  400000.0
#define TERRAIN_HILL_WAVELENGTH_M       300000.0
#define TERRAIN_HYPSO_GAMMA             1.3

/* The land's roughness spectrum, one for the whole planet and every scale:
   octave amplitude A(lambda) = A10 x (lambda / 10 km)^H up to the knee
   (below), between the plains' and the mountains' by ruggedness (A10
   geometrically). H = 1
   keeps the slope the same at every scale; lower is rougher at short
   wavelengths. The bake carries it from TERRAIN_ROUGH_MAP_WAVELENGTH_M down
   to two map texels (heightbake.frag, in metres via bake_job.c); the
   detail noise carries it below the map's texel (detail.glsl). Values for
   ~2x Earth (docs/terrain-relief.md). */
#define TERRAIN_ROUGH_A10_PLAINS_M      80.0
#define TERRAIN_ROUGH_A10_MOUNTAIN_M    1500.0
#define TERRAIN_ROUGH_H_PLAINS          1.0
#define TERRAIN_ROUGH_H_MOUNTAIN        0.9
/* Above the knee wavelength the amplitude grows only as lambda^H_LONG:
   plains are flat at long wavelengths (their roughness is short: gullies,
   low hills), mountains rough at every scale (their knee is past the map).
   The knee goes between the two geometrically by ruggedness. */
#define TERRAIN_ROUGH_KNEE_PLAINS_M     5000.0
#define TERRAIN_ROUGH_KNEE_MOUNTAIN_M   1000000.0
#define TERRAIN_ROUGH_H_LONG            0.2
#define TERRAIN_ROUGH_MAP_WAVELENGTH_M  50000.0
/* Below this wavelength (m) the amplitude falls with the wavelength (the
   slope stops growing: rock, not ever-steeper fractal spikes). */
#define TERRAIN_ROUGH_LAMBDA_C_M        100.0

/* ---- Baked terrain maps (terrain_gen.c, shaders/planetmap.glsl) ------------
   Height, climate and ocean maps are cube maps over the body-local sphere:
   GL's face layout with the plain (gnomonic) projection the quadtree uses,
   so a face's texel grid lines up with its quadtree nodes. Texels per face
   edge (power of two). 1024: ~12 km at face centres on Earth, down to ~4 km
   toward the corners. Bake time and map memory grow with its square. */
#define TERRAIN_MAP_SIZE          1024

/* ---- Terrain detail (shaders/detail.glsl, terrain_detail.c) ---------------
   Fractal noise on land below the baked height map's resolution. Each
   octave halves the wavelength; octaves fade in with camera distance, as
   geometry while the triangles can carry them and as shading normals down
   to ~2 px. Noise stays off the seafloor and ramps in above the shore so
   land never dips below sea level.
   It continues the land's roughness spectrum (TERRAIN_ROUGH_*, above)
   below the map: the coarsest octave is one map texel long (its lattice is
   the map's texel grid; the bake carries the longer ones), each octave's
   amplitude the spectrum's at its wavelength, by the ruggedness there:
   from the baked map's slope (rise / run, TERRAIN_RUGGED_SLOPE_LO .. _HI)
   or its height (fraction of the relief, TERRAIN_RUGGED_ALT_LO .. _HI,
   up to 0.6 from height alone). */
/* x every amplitude (debug panel "Detail roughness"). */
#define TERRAIN_DETAIL_ROUGHNESS        1.0
/* Mountain shape (detail.glsl): rugged ground's octaves are ridged and
   damped by the coarser octaves' slope under them, 1 / (1 + ERODE x
   ruggedness x slope^2) (slope as rise / run): their steep flanks
   smoother, gentle ground detailed. */
#define TERRAIN_DETAIL_ERODE            2.0
#define TERRAIN_RUGGED_SLOPE_LO         0.01
#define TERRAIN_RUGGED_SLOPE_HI         0.15
#define TERRAIN_RUGGED_ALT_LO           0.3
#define TERRAIN_RUGGED_ALT_HI           0.8
/* Finest octave's wavelength (m); no finer than ~QT_MIN_VERTEX_SPACING_M
   matters for geometry, finer still only for close-up shading. */
#define TERRAIN_DETAIL_MIN_WAVELENGTH_M 1.0
/* Detail shaded per pixel below the triangle size (normals only, up to 3
   octaves, down to half a triangle edge). 0 = the vertices' slope only.
   The most expensive part of the detail: about +9 ms for the first octave
   and +4 ms for each further one with land filling the screen on an
   integrated GPU at 2880x1880. Compiled into quad.frag (restart to change). */
#define TERRAIN_DETAIL_PIXEL_OCTAVES    3

/* Surface materials by height in metres (quad.frag), the same on any
   planet (ground scale). Temperature is quad.frag's unit: 1 at the equator
   at sea level, -0.2 at the poles, snow below PlanetStyle.snowTemp (0.12
   on Earth), forest above treeTemp (0.33). The lapse puts Earth's
   equatorial snowline at (1 - 0.12) / LAPSE = 10 km and its treeline at
   ~7.6 km; at 45 deg ~6 and ~3.6 km, at 60 deg ~3.2 and ~0.8 km. That is
   2x real, as the relief: ranges keep Earth's share of snow (at the real
   5 km, 10% of the land is above it and whole massifs are white). Dry
   ground lapses x (1 - LAPSE_DRY), its snowline higher (as the dry
   Andes'). */
#define TERRAIN_LAPSE_PER_KM            0.088
#define TERRAIN_LAPSE_DRY               0.2

/* ---- Atmosphere (atmosphere.c, atmosphere.frag) -------------------------
   A stylised single-scattering shell (O'Neill, GPU Gems 2 ch. 16), tuned
   for look rather than measured Earth (which is ~100 km with 8 km air and
   1.2 km haze scale heights), thinned from the original 270 km / 67.5 km
   toward it when the relief became real (2026-10-23): the sky's colours
   are about the same (the dome's optical depths are per shell height),
   the limb a thinner, crisper band. The debug panel's Atmosphere section
   edits both live.
   Shell height: where the air ends. The scattering constants (K_R, K_M)
   are per shell height, so a taller shell also scatters more overall.
   Scale height: density falls by e every this many metres. Below ~1/8 of
   the shell the dome's 5-sample ray march starts to band. */
#define ATMOSPHERE_SHELL_HEIGHT_M   120000.0
/* Scale height as a fraction of the shell (generated planets vary it
   x0.8-1.3, staying above the banding limit). */
#define ATMOSPHERE_SCALE_FRACTION   (1.0 / 6.0)
#define ATMOSPHERE_SCALE_HEIGHT_M   (ATMOSPHERE_SCALE_FRACTION * ATMOSPHERE_SHELL_HEIGHT_M) /* 20 km */

/* ---- Clouds (clouds.c, the shared cloud volume, docs/volumetric-clouds.md)
   Earth's cloud layer; every other planet's is generated (solar_system.c)
   from the ranges further down. The debug panel's Clouds section edits the
   active planet's live. One volumetric layer over the altitude +- half the
   thickness, the same clouds from the ground to orbit.
   Weather scale: the largest weather system's wavelength, in planet radii.
   Opacity: optical depth of a full cloud column (thick cumulus ~20-50).
   Flow period: simulation seconds per cycle of the two-phase flow map that
   moves the clouds along the wind map (clouds re-form over about this).
   Keep wind x period well under the weather scale: winds differ between
   latitude belts, and a long period shears the noise into streaks.
   Heights are real: Earth's layer runs from ~1 km (the cumulus bases) to
   ~9 km (where storms top out); how high a cloud builds inside it is the
   weather's (fair-weather cumulus stay in the bottom part). The highest
   ranges stand at its top. */
/* Clouds start switched on; the debug panel's Clouds section (or
   --clouds 0) turns them off. */
#define CLOUDS_ENABLED_DEFAULT      1
#define CLOUD_EARTH_ALTITUDE_M      5000.0
#define CLOUD_EARTH_THICKNESS_M     8000.0
#define CLOUD_EARTH_COVERAGE        0.5
#define CLOUD_EARTH_CLIMATE_WEIGHT  0.8
#define CLOUD_EARTH_OPACITY         40.0
#define CLOUD_EARTH_WEATHER_SCALE   0.3
#define CLOUD_EARTH_FLOW_PERIOD_S   (0.5 * 86400.0)
/* Weather maps (equirect, like the climate map): width in texels, and the
   simulation time between rebuilds (the two newest are crossfaded). At
   most this many maps are built per frame across all planets. */
#define CLOUD_MAP_WIDTH             1024
#define CLOUD_MAP_STEP_S            3600.0
#define CLOUD_MAP_BUILDS_PER_FRAME  2
/* Detail below the weather maps, baked into the weather cube: octaves
   and the finest wavelength (m). */
#define CLOUD_DETAIL_OCTAVES        9
#define CLOUD_DETAIL_MIN_WAVELENGTH_M 200.0
/* Generated planets: layer altitude as a fraction of the planet's land
   relief (Earth's is ~0.56), thickness as a fraction of the altitude
   (Earth's 1.6: the bases low down). Overcast (Venus-like) decks sit well
   above the highest peaks. Every layer is kept under CLOUD_GEN_MAX_SHELL
   of the atmosphere shell. */
#define CLOUD_GEN_ALTITUDE_MIN      0.35
#define CLOUD_GEN_ALTITUDE_MAX      0.8
#define CLOUD_GEN_OVERCAST_ALT_MIN  2.5
#define CLOUD_GEN_OVERCAST_ALT_MAX  5.0
#define CLOUD_GEN_THICKNESS_MIN     0.8
#define CLOUD_GEN_THICKNESS_MAX     1.6
#define CLOUD_GEN_MAX_SHELL         0.6
/* The active planet's weather cube (clouds.c, cloudweather.frag): texels a
   face side (2048: ~6 km on Earth), re-made as its
   weather maps cross-fade every CLOUD_VOL_REBAKE_BLEND of a map step. The
   volume's shapes are CLOUD_VOL_SCALE thicknesses across, its extinction
   CLOUD_VOL_DENSITY x opacity / thickness. */
#define CLOUD_CUBE_SIZE             2048
#define CLOUD_VOL_REBAKE_BLEND      0.05
#define CLOUD_VOL_SCALE             0.7
#define CLOUD_VOL_DENSITY           1.2
/* The planet clouds' sky and ground light (clouds.c, cloudsky.frag): a
   table of CLOUD_SKY_LUT_SIZE Sun heights, made from the dome's scattering;
   the ground under the clouds bounces CLOUD_GROUND_ALBEDO of its light
   (sea and land together, linear). */
#define CLOUD_SKY_LUT_SIZE          64
/* x the table's sky and ground light on the clouds: the dome scatters
   once, so its sky is darker and bluer than the real one. */
#define CLOUD_SKY_GAIN              1.8
#define CLOUD_GROUND_ALBEDO_R       0.11
#define CLOUD_GROUND_ALBEDO_G       0.11
#define CLOUD_GROUND_ALBEDO_B       0.10

/* ---- Nebula (nebula.c, nebula*.glsl/frag) --------------------------------
   One density function in nebula units n = (p - sun) / NEBULA_RADIUS_M,
   in two layers that never overlap (.kilo/plans/...-volumetric-nebula-
   skybox.md):
   - near: wisps inside |n| < 1, ray-marched every frame with real
     parallax (NEBULA_ENABLE_DEFAULT; debug panel);
   - far: a shell |n| in [FAR_START, FAR_END], baked into the skybox
     cubemap as seen from the sun (cube.exe --bake-sky <name>, or the debug
     panel's Rebake). Its parallax error for a camera at |n| = c is about
     c / FAR_START radians.
   Nebula radius: about the generated system's size (CAMERA_DISTANCE_MAX_M
   is 2e12). Both switches start off so bench references stay
   pixel-identical: an empty sky name keeps assets/textures/skybox3. */
#define NEBULA_ENABLE_DEFAULT       0
#define NEBULA_SKY_NAME             "eve2"
#define NEBULA_RADIUS_M             1.0e12
#define NEBULA_FAR_START            60.0
#define NEBULA_FAR_END              200.0
#define NEBULA_SEED_DEFAULT         1u
/* Bake: face size (texels), coarse march steps through the far shell's
   empty space (inside clouds the march steps a pixel's footprint), and
   the rows drawn per strip (each strip is one draw, kept short of the 2 s
   Windows GPU timeout on an integrated GPU). */
#define NEBULA_SKY_FACE_SIZE        2048
#define NEBULA_BAKE_STEPS           128
#define NEBULA_BAKE_STRIP_ROWS      32
/* Runtime: march steps through the near sphere (a chord of at most 2),
   and the presence grid's texels per side over n in [-1, 1]^3. */
#define NEBULA_VOL_STEPS            48
#define NEBULA_GRID_SIZE            64
/* Float32 lattice exactness: the noise coordinate n / wavelength stays
   under ~1e4 when the wavelengths are kept above this (nebula units). */
#define NEBULA_MIN_WAVELENGTH       1.0e-3

/* ---- Ocean ---------------------------------------------------------------
   Geometric swell: rms height (m) the water shell's vertices are displaced
   by at "Wave height" 1.0 (debug panel). Shading (slopes, foam) uses the
   normalised wave field and does not change with it. The FFT field itself
   is calibrated in body radii (water.c), so without this the swell would
   be kilometres tall at Earth scale. */
#define OCEAN_SWELL_RMS_M         2.0
/* FFT wave cascades (water.c, Tessendorf): OCEAN_CASCADES patches, the
   largest OCEAN_WAVE_PATCH_M on a side and each next one OCEAN_CASCADE_RATIO
   smaller (1000, 173, 30 m), each simulating its own band of wavelengths.
   The ratio is deliberately not an integer, so the patches' periods never
   line up and their sum does not visibly repeat. The water shaders tile
   them over the cube faces (wavecube.glsl) at this size times the debug
   panel's "Wave scale" (default 10: stylized, larger seas; heights scale
   with it and time by its square root, so the waves keep their shape and
   a believable speed). The finest cascade stays at true size, for close-up
   chop (Water_CascadeScale, water.c). From orbit they filter into the
   glint's roughness.
   OCEAN_CASCADES must match WAVE_CASCADES in wavecube.glsl. */
#define OCEAN_CASCADES            3
#define OCEAN_WAVE_PATCH_M        1000.0
#define OCEAN_CASCADE_RATIO       5.77

/* ---- Rendering ------------------------------------------------------------
   MSAA samples of the main scene target. Every sample costs memory
   bandwidth (and the planet's depth prepass runs per sample). */
#define RENDER_MSAA_SAMPLES       2

/* ---- Sun rendering ------------------------------------------------------
   The sun billboard's corona half-size, in sun radii. The opaque disc is
   ~0.175 of it, so ~5.7 draws the disc at the sun's true apparent size;
   95 keeps the old (stylised, larger) look. */
#define SUN_GLOW_RADII            95.0
/* Sun angular radius (radians) used for shadow penumbrae (ring shadow,
   sunset cut-off). The true value at 1 AU is 0.00465. */
#define SUN_ANGULAR_RADIUS_RAD    0.012

/* ---- Shadows ------------------------------------------------------------
   One directional (orthographic) shadow map around the planet. The box is
   the planet's bounding sphere times the margin; its depth spans the
   moon's orbit on both sides so eclipses work both ways. */
#define SHADOW_MAP_SIZE           4096
#define SHADOW_BOUND_MARGIN       1.02

/* Cascades (shadows.c, shaders/shadow.glsl): finer maps around the camera,
   each fitted to a slice of the view distance, from the nearest visible
   terrain out to SHADOW_CASCADE_MAX_DIST_M with logarithmic splits (every
   cascade covers the same distance ratio). Past them, and from far orbit
   where the range is too short to beat the global map, the global map
   above is used. Casters are drawn again per cascade with the main pass's
   LOD, so each cascade costs about a planet depth pass over its area.
   Memory: CASCADES x SIZE^2 x 4 bytes (64 MB at 4 x 2048). */
#define SHADOW_CASCADES             4      /* 0-4; 0 = global map only */
#define SHADOW_CASCADE_SIZE         2048
#define SHADOW_CASCADE_MAX_DIST_M   1.5e6  /* last cascade ~1.1 km texels */
/* Never start the splits nearer than this (m): standing on the ground the
   first cascade then spans ~60 m at ~4 cm texels. */
#define SHADOW_CASCADE_MIN_DIST_M   2.0

/* ---- Rings: close-up field (metres) -------------------------------------
   Rock + dust streaming around the camera (rings.c). The ring radii
   themselves are in planet radii in rings.c. These keep the old look;
   real ring particles are cm to ~10 m. */
#define RING_ROCK_RANGE_M         1.5e6
#define RING_ROCK_CELL_M          7.5e4
#define RING_ROCK_MIN_SIZE_M      2.5e3
#define RING_ROCK_MAX_SIZE_M      5.3e4
#define RING_HALF_THICKNESS_M     6.4e4
#define RING_DUST_RANGE_M         6.36e5
#define RING_DUST_CELL_M          2.12e4
#define RING_DUST_HALF_THICKNESS_M 4.2e4

#endif
