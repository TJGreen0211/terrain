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
   Read by the body table in Bodies.c. Earth is the scene origin for now;
   the sun and moon are placed relative to it (heliocentric orbits come
   later). Orbits are circles in the world XZ plane:
       position = parent + radius * (cos(phase), 0, sin(phase)),
       phase    = ORBIT_PHASE + 2*pi * t / ORBIT_PERIOD   (period 0 = fixed).
   Rotation (body -> world) = rotateX(AXIAL_TILT - 90) * rotateZ(spin): the
   body's local +Z pole is tipped up to world +Y, then leaned by the tilt.
   spin = ROTATION_PHASE + 360 * t / ROTATION_PERIOD. */

#define SUN_RADIUS_M              6.957e8
#define SUN_DISTANCE_M            AU_M
#define SUN_ORBIT_PHASE_DEG       225.0      /* direction (-1, 0, -1) from Earth */

#define EARTH_RADIUS_M            6.371e6
#define EARTH_ROTATION_PERIOD_S   86164.0905 /* sidereal day */
#define EARTH_AXIAL_TILT_DEG      23.44

#define MOON_RADIUS_M             1.7374e6
#define MOON_ORBIT_RADIUS_M       3.844e8
#define MOON_ORBIT_PERIOD_S       2360591.5  /* 27.32 days, sidereal */
#define MOON_ORBIT_PHASE_DEG      0.0
#define MOON_AXIAL_TILT_DEG       1.54
/* Tidally locked: one spin per orbit. */
#define MOON_ROTATION_PERIOD_S    MOON_ORBIT_PERIOD_S

/* ---- Time --------------------------------------------------------------
   Simulation seconds per real second (R pauses). 1440 = one Earth day per
   real minute, close to the old per-frame spin at 60 fps. */
#define SIM_TIME_SCALE            1440.0

/* ---- Camera (metres) ---------------------------------------------------- */

/* Main camera near plane. The main pass is infinite reversed-Z into a float
   depth buffer, so this can sit at surface scale. */
#define CAMERA_NEAR_M             0.1
/* Orbit pivot spawn, on +Z from the planet centre. */
#define CAMERA_SPAWN_DISTANCE_M   (3.0 * EARTH_RADIUS_M)
/* Scroll-out limit of the orbit radius behind the pivot. */
#define CAMERA_DISTANCE_MAX_M     (20.0 * EARTH_RADIUS_M)
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
