#include "Bodies.h"
#include "WorldConfig.h"
#include <math.h>

/* The body table. Values come from WorldConfig.h; edit them there. */
Body g_bodies[BODY_COUNT] = {
	[BODY_EARTH] = {
		.name = "Earth",
		.radius = EARTH_RADIUS_M,
		.parent = -1,
		.rotationPeriod = EARTH_ROTATION_PERIOD_S,
		.axialTilt = EARTH_AXIAL_TILT_DEG,
	},
	[BODY_SUN] = {
		.name = "Sun",
		.radius = SUN_RADIUS_M,
		.parent = BODY_EARTH,
		.orbitRadius = SUN_DISTANCE_M,
		.orbitPhase = SUN_ORBIT_PHASE_DEG,
	},
	[BODY_MOON] = {
		.name = "Moon",
		.radius = MOON_RADIUS_M,
		.parent = BODY_EARTH,
		.orbitRadius = MOON_ORBIT_RADIUS_M,
		.orbitPeriod = MOON_ORBIT_PERIOD_S,
		.orbitPhase = MOON_ORBIT_PHASE_DEG,
		.rotationPeriod = MOON_ROTATION_PERIOD_S,
		.axialTilt = MOON_AXIAL_TILT_DEG,
	},
};

/* Phase (degrees) after t seconds of a cycle with the given period, kept in
   [0, 360) before it ever reaches sin/cos so long runs never lose bits. */
static double cyclePhase(double phase0, double period, double t)
{
	if (period == 0.0)
		return phase0;
	return phase0 + 360.0 * fmod(t / period, 1.0);
}

void Bodies_Update(double t)
{
	for (int i = 0; i < BODY_COUNT; i++) {
		Body *b = &g_bodies[i];

		dvec3 origin = { 0.0, 0.0, 0.0 };
		if (b->parent >= 0)
			origin = g_bodies[b->parent].position;
		double a = cyclePhase(b->orbitPhase, b->orbitPeriod, t) * (M_PI / 180.0);
		dvec3 offset = { b->orbitRadius * cos(a), 0.0, b->orbitRadius * sin(a) };
		b->position = daddvec3(origin, offset);

		/* Spin about the body's own +Z pole, then tip the pole up to world
		   +Y and lean it by the axial tilt. */
		double spin = cyclePhase(b->rotationPhase, b->rotationPeriod, t);
		b->rotation = dmultiplymat4(drotateX(b->axialTilt - 90.0), drotateZ(spin));
	}
}

dmat4 Body_ModelMatrix(const Body *b, dvec3 cam)
{
	dmat4 t = dtranslatevec3(dsubvec3(b->position, cam));
	return dmultiplymat4(t, dmultiplymat4(dscalemat4(b->radius), b->rotation));
}
