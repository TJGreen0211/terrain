#ifndef WORLDCONFIG_H
#define WORLDCONFIG_H

/* ======================================================================
   WORLD CONFIGURATION — every world-scale constant lives here.

   Units: 1 world unit = 1 metre. Angles in degrees unless the name says
   _RAD. Times in seconds of simulation time.

   Precision rule: positions, orbits, rotations and time are double on the
   CPU. The GPU only ever sees camera-relative (or body-local) floats; see
   Bodies.c and camRelTranslate in Main.c.

   Shading / look constants that are already expressed as fractions of a
   body radius (atmosphere shell, terrain relief, ring radii, ...) stay with
   the code that uses them, since they do not change with scale.
   ====================================================================== */

/* ---- Physical constants ------------------------------------------------ */

#define AU_M 1.495978707e11

/* ---- Bodies ------------------------------------------------------------
   The Sun is the world origin; every other body follows a Kepler ellipse
   around its parent (Bodies.c). Earth (the Earth-like planet we build on)
   and its Moon are fixed here; every other planet is generated from the
   system seed (SolarSystem.c, "Procedural system" below).

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

/* ---- Procedural system (SolarSystem.c) ----------------------------------
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
/* Rocky planets: atmosphere shell as a fraction of the radius (Earth's look
   is ATMOSPHERE_SHELL_HEIGHT_M / EARTH_RADIUS_M), times a random factor. */
#define ROCKY_SHELL_FRACTION      (ATMOSPHERE_SHELL_HEIGHT_M / EARTH_RADIUS_M)
/* Water on every planet but Earth: generated (hasWater) but not drawn
   until the water rendering is reworked. 0 = dry worlds. */
#define OTHER_PLANET_WATER        0

/* ---- Time --------------------------------------------------------------
   Simulation seconds per real second (R pauses). 1440 = one Earth day per
   real minute, close to the old per-frame spin at 60 fps. The default of
   the debug panel's time-scale slider; bench runs always use it. */
#define SIM_TIME_SCALE            1440.0
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

/* ---- Terrain detail (shaders/detail.glsl, TerrainDetail.c) ---------------
   Fractal noise on land below the baked height map's resolution (~20 km
   per texel on Earth). Each octave halves the wavelength; octaves fade in
   with camera distance, as geometry while the triangles can carry them and
   as shading normals down to ~2 px. Noise stays off the seafloor and ramps
   in above the shore so land never dips below sea level. */
/* Coarsest octave's wavelength (m); snapped to a power-of-two lattice on
   the cube face (12.4 km on Earth). */
#define TERRAIN_DETAIL_WAVELENGTH_M     12000.0
/* Its amplitude (m). Peak detail is about twice this (all octaves). */
#define TERRAIN_DETAIL_AMPLITUDE_M      400.0
/* Amplitude ratio from one octave to the next (0.5 keeps the slope
   similar at every scale; higher = rougher close up). */
#define TERRAIN_DETAIL_GAIN             0.5
/* Finest octave's wavelength (m); no finer than ~QT_MIN_VERTEX_SPACING_M
   matters for geometry, finer still only for close-up shading. */
#define TERRAIN_DETAIL_MIN_WAVELENGTH_M 1.0
/* Amplitude on low land (plains, coasts) relative to high ground. */
#define TERRAIN_DETAIL_PLAINS           0.3
/* Detail shaded per pixel below the triangle size (normals only, up to 3
   octaves, down to half a triangle edge). 0 = the vertices' slope only.
   The most expensive part of the detail: about +9 ms for the first octave
   and +4 ms for each further one with land filling the screen on an
   integrated GPU at 2880x1880. Compiled into quad.frag (restart to change). */
#define TERRAIN_DETAIL_PIXEL_OCTAVES    3

/* ---- Atmosphere (Atmosphere.c, atmosphere.frag) -------------------------
   A stylised single-scattering shell (O'Neill, GPU Gems 2 ch. 16), tuned
   for look rather than measured Earth (which is ~100 km with 8 km air and
   1.2 km haze scale heights). Defaults reproduce the original look. The
   debug panel's Atmosphere section edits both live.
   Shell height: where the air ends. The scattering constants (K_R, K_M)
   are per shell height, so a taller shell also scatters more overall.
   Scale height: density falls by e every this many metres. Below ~1/8 of
   the shell the dome's 5-sample ray march starts to band. */
#define ATMOSPHERE_SHELL_HEIGHT_M   (0.0424 * EARTH_RADIUS_M)          /* 270 km */
#define ATMOSPHERE_SCALE_HEIGHT_M   (0.25 * ATMOSPHERE_SHELL_HEIGHT_M) /* 67.5 km */

/* ---- Ocean ---------------------------------------------------------------
   Geometric swell: rms height (m) the water shell's vertices are displaced
   by at "Wave height" 1.0 (debug panel). Shading (slopes, foam) uses the
   normalised wave field and does not change with it. The FFT field itself
   is calibrated in body radii (Water.c), so without this the swell would
   be kilometres tall at Earth scale. */
#define OCEAN_SWELL_RMS_M         2.0

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

/* Cascades (Shadows.c, shaders/shadow.glsl): finer maps around the camera,
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
   Rock + dust streaming around the camera (Rings.c). The ring radii
   themselves are in planet radii in Rings.c. These keep the old look;
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
