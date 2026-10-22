#ifndef BODIES_H
#define BODIES_H

#include "engine/math/linalg.h"

/* Celestial bodies: physical description (from WorldConfig.h) plus the
   per-frame state derived from the simulation time. Everything is double
   and in metres; the GPU only sees Body_ModelMatrix after the camera is
   subtracted and it is downcast.

   The Sun is the world origin. A body's parent must come earlier in the
   table (parents update first). Sun, Earth and the Moon are fixed; the
   other entries are filled by SolarSystem_Generate from the system seed. */
typedef enum BodyId {
	BODY_SUN,
	BODY_MERCURY,
	BODY_VENUS,
	BODY_EARTH,
	BODY_MOON,
	BODY_MARS,
	BODY_CERES,
	BODY_JUPITER,
	BODY_SATURN,
	BODY_URANUS,
	BODY_NEPTUNE,
	BODY_PLUTO,
	BODY_HAUMEA,
	BODY_MAKEMAKE,
	BODY_COUNT
} BodyId;

/* How a body is drawn. */
typedef enum BodyKind {
	BODY_KIND_STAR,      /* the light (sun.c disc + glow) */
	BODY_KIND_TERRAIN,   /* full planet: quadtree terrain, atmosphere, rings (water: Earth) */
	BODY_KIND_TEXTURED,  /* textured smooth sphere (the Moon) */
	BODY_KIND_SIMPLE     /* shaded sphere / dot only (planets.c) */
} BodyKind;

/* Kepler elements at simTime 0, in the order of WorldConfig.h's _ORBIT
   lists. */
typedef struct OrbitElements {
	double a;        /* semi-major axis, m (before ORBIT_DISTANCE_SCALE) */
	double e;        /* eccentricity */
	double i;        /* inclination, deg */
	double node;     /* longitude of the ascending node, deg */
	double peri;     /* longitude of perihelion, deg */
	double L;        /* mean longitude at t = 0, deg */
	double period;   /* s; 0 = Kepler's third law around the Sun */
} OrbitElements;

typedef struct Body {
	const char *name;
	BodyKind kind;
	double radius;          /* m (as drawn) */
	int parent;             /* BodyId, or -1 = the world origin */
	OrbitElements orbit;
	double rotationPeriod;  /* s; 0 = no spin */
	double axialTilt;       /* deg from world +Y */
	float color[3];         /* surface / cloud-top colour (simple bodies) */
	int banded;             /* gas giant: latitude bands */

	/* Per frame (Bodies_Update). */
	dvec3 position;         /* world, m */
	dmat4 rotation;         /* body -> world, rotation only */
} Body;

/* Body id (BodyId). */
Body *Bodies_Get(int id);

typedef struct BodiesParams {
	/* Live simulation seconds per real second (debug panel); starts at
	   SIM_TIME_SCALE. */
	double timeScale;
	/* The body the camera follows (Tab / debug panel / bench; the app
	   applies a change). */
	int focusBody;
} BodiesParams;
BodiesParams *Bodies_Params(void);
/* Simulation time of the current frame (s since J2000). */
double Bodies_SimTime(void);

/* Positions and orientations at simulation time t (seconds). */
void Bodies_Update(double t);

/* translate(position - cam) * scale(radius) * rotation, in double: the
   camera-relative model matrix of a unit-radius mesh. Downcast with
   dmat4tomat4 right before upload. */
dmat4 Body_ModelMatrix(const Body *b, dvec3 cam);

#endif
