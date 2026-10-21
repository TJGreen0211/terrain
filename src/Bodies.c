#include "Bodies.h"
#include "WorldConfig.h"
#include <math.h>

/* The body table. Sun, Earth and the Moon come from WorldConfig.h; the
   other planets are names only until SolarSystem_Generate fills them in
   from the system seed. Colours are look only (Planets.c shading). */
Body g_bodies[BODY_COUNT] = {
	[BODY_SUN] = {
		.name = "Sun", .kind = BODY_KIND_STAR,
		.radius = SUN_RADIUS_M * ORBIT_DISTANCE_SCALE,
		.parent = -1,
		.color = { 1.0f, 0.95f, 0.85f },
	},
	[BODY_MERCURY]  = { .name = "Mercury",  .parent = BODY_SUN },
	[BODY_VENUS]    = { .name = "Venus",    .parent = BODY_SUN },
	[BODY_EARTH] = {
		.name = "Earth", .kind = BODY_KIND_TERRAIN,
		.radius = EARTH_RADIUS_M, .parent = BODY_SUN, .orbit = { EARTH_ORBIT },
		.rotationPeriod = EARTH_ROTATION_PERIOD_S, .axialTilt = EARTH_AXIAL_TILT_DEG,
		.color = { 0.30f, 0.45f, 0.70f },
	},
	[BODY_MOON] = {
		.name = "Moon", .kind = BODY_KIND_TEXTURED,
		.radius = MOON_RADIUS_M, .parent = BODY_EARTH, .orbit = { MOON_ORBIT },
		.rotationPeriod = MOON_ROTATION_PERIOD_S, .axialTilt = MOON_AXIAL_TILT_DEG,
		.color = { 0.55f, 0.55f, 0.53f },
	},
	[BODY_MARS]     = { .name = "Mars",     .parent = BODY_SUN },
	[BODY_CERES]    = { .name = "Ceres",    .parent = BODY_SUN },
	[BODY_JUPITER]  = { .name = "Jupiter",  .parent = BODY_SUN },
	[BODY_SATURN]   = { .name = "Saturn",   .parent = BODY_SUN },
	[BODY_URANUS]   = { .name = "Uranus",   .parent = BODY_SUN },
	[BODY_NEPTUNE]  = { .name = "Neptune",  .parent = BODY_SUN },
	[BODY_PLUTO]    = { .name = "Pluto",    .parent = BODY_SUN },
	[BODY_HAUMEA]   = { .name = "Haumea",   .parent = BODY_SUN },
	[BODY_MAKEMAKE] = { .name = "Makemake", .parent = BODY_SUN },
};

double g_simTimeScale = SIM_TIME_SCALE;
double g_simTime = 0.0;
int g_focusBody = BODY_EARTH;

static const double DEG = M_PI / 180.0;

/* Fraction of a cycle after t seconds, in [0, 1), taken before it ever
   reaches sin/cos so long runs never lose bits. */
static double cycles(double period, double t)
{
	return period == 0.0 ? 0.0 : fmod(t / period, 1.0);
}

/* Sidereal period (s) of an orbit: its own, or Kepler's third law around
   the Sun from the real semi-major axis. */
static double orbitPeriod(const OrbitElements *o)
{
	return o->period > 0.0 ? o->period : SIDEREAL_YEAR_S * pow(o->a / AU_M, 1.5);
}

/* Position relative to the parent (world axes, before the ecliptic turn)
   at time t: Kepler's equation by Newton iteration, then perifocal ->
   ecliptic -> world (ecliptic north = +Y, counter-clockwise from +Y). */
static dvec3 keplerPosition(const OrbitElements *o, double a, double t)
{
	double M = fmod((o->L - o->peri) * DEG + 2.0 * M_PI * cycles(orbitPeriod(o), t), 2.0 * M_PI);
	if (M < 0.0)
		M += 2.0 * M_PI;
	double e = o->e;
	double E = e < 0.8 ? M : M_PI;
	for (int k = 0; k < 12; k++) {
		double dE = (E - e * sin(E) - M) / (1.0 - e * cos(E));
		E -= dE;
		if (fabs(dE) < 1e-14)
			break;
	}
	double xp = a * (cos(E) - e);
	double yp = a * sqrt(1.0 - e * e) * sin(E);

	double w = (o->peri - o->node) * DEG, N = o->node * DEG, i = o->i * DEG;
	double cw = cos(w), sw = sin(w), cN = cos(N), sN = sin(N), ci = cos(i), si = sin(i);
	double xe = (cw * cN - sw * sN * ci) * xp + (-sw * cN - cw * sN * ci) * yp;
	double ye = (cw * sN + sw * cN * ci) * xp + (-sw * sN + cw * cN * ci) * yp;
	double ze = (sw * si) * xp + (cw * si) * yp;
	dvec3 p = { xe, ze, -ye };
	return p;
}

static double semiMajor(const Body *b)
{
	/* Compression applies to the Sun's children only. */
	return b->parent == BODY_SUN ? b->orbit.a * ORBIT_DISTANCE_SCALE : b->orbit.a;
}

/* World XZ turn applied to every orbit: puts the Sun at SUN_ORBIT_PHASE_DEG
   as seen from Earth at t = 0. */
static double eclipticTurn(void)
{
	static int ready;
	static double turn;
	if (!ready) {
		const Body *e = &g_bodies[BODY_EARTH];
		dvec3 p = keplerPosition(&e->orbit, semiMajor(e), 0.0);
		double want = SUN_ORBIT_PHASE_DEG * DEG + M_PI;   /* Earth seen from the Sun */
		turn = want - atan2(p.z, p.x);
		ready = 1;
	}
	return turn;
}

void Bodies_Update(double t)
{
	g_simTime = t;
	double turn = eclipticTurn();
	double ct = cos(turn), st = sin(turn);

	for (int i = 0; i < BODY_COUNT; i++) {
		Body *b = &g_bodies[i];

		dvec3 origin = { 0.0, 0.0, 0.0 };
		dvec3 offset = { 0.0, 0.0, 0.0 };
		if (b->parent >= 0) {
			origin = g_bodies[b->parent].position;
			dvec3 p = keplerPosition(&b->orbit, semiMajor(b), t);
			offset.x = p.x * ct - p.z * st;
			offset.y = p.y;
			offset.z = p.x * st + p.z * ct;
		}
		b->position = daddvec3(origin, offset);

		/* Spin about the body's own +Z pole, then tip the pole up to world
		   +Y and lean it by the axial tilt. */
		double spin = 360.0 * cycles(b->rotationPeriod, t);
		b->rotation = dmultiplymat4(drotateX(b->axialTilt - 90.0), drotateZ(spin));
	}
}

dmat4 Body_ModelMatrix(const Body *b, dvec3 cam)
{
	dmat4 t = dtranslatevec3(dsubvec3(b->position, cam));
	return dmultiplymat4(t, dmultiplymat4(dscalemat4(b->radius), b->rotation));
}
