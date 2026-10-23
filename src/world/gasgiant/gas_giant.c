#include "world/gasgiant/gas_giant.h"
#include "world/gasgiant/gas_interior.h"
#include "world/bodies/bodies.h"
#include "world/bodies/solar_system.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"
#include "world/WorldConfig.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Texture units in the stand-in program (clouds use 0 and 1, the ring
   profile 9). */
#define GASGIANT_PROFILE_UNIT 10
#define GASGIANT_NOISE_UNIT   11
/* Latitude profile texels, pole to pole. */
#define PROFILE_N 1024
/* Noise texture: NOISE_N^3 texels tiling NOISE_PERIOD lattice cells
   (gasgiant.glsl's GAS_NOISE_PERIOD), 4 texels per cell, so hardware
   trilinear between the smooth samples stays smooth. */
#define NOISE_N      128
#define NOISE_PERIOD 32

static GLuint noiseTex;

static GasGiantParams g_params = {
	1.35f,                    /* limbK */
	0.18f,                    /* wrap */
	{ 1.0f, 0.62f, 0.38f },   /* terminator */
	0.006f,                   /* ambient */
	1.0f,                     /* rim */
	1.0f,                     /* glow */
	1.05f,                    /* exposure */
	3.0f,                     /* lightning */
	4.0f,                     /* flash */
	(float)GAS_FLASH_SIZE_M,  /* flashSize */
	0,                        /* flashHold */
	0.45f,                    /* aurora */
	0.35f,                    /* auroraDay */
	0.3f,                     /* auroraSpeed */
	{ 0.35f, 1.0f, 0.55f },   /* auroraLow */
	{ 0.58f, 0.32f, 1.0f },   /* auroraMid */
	{ 1.0f, 0.3f, 0.82f },    /* auroraHigh */
	0.6f,                     /* auroraGreen */
	0.4f,                     /* auroraLean */
	90.0f,                    /* auroraCycle */
	0,                        /* auroraAlways */
};

static double g_time;

void GasGiant_SetTime(double sceneTime)
{
	g_time = sceneTime;
}

GasGiantParams *GasGiant_Params(void)
{
	return &g_params;
}

const char *GasGiant_FamilyName(int family)
{
	switch (family) {
	case GASGIANT_JOVIAN:    return "Jovian";
	case GASGIANT_SATURNIAN: return "Saturnian";
	case GASGIANT_ICE:       return "Ice giant";
	case GASGIANT_EXOTIC:    return "Exotic";
	default:                 return "?";
	}
}

/* ---- Random numbers (as solar_system.c's) ---- */

typedef struct { unsigned long long s; } Rng;

static unsigned long long splitmix(unsigned long long *s)
{
	unsigned long long z = (*s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static Rng rngFor(unsigned int seed, int stream)
{
	Rng r = { ((unsigned long long)seed << 32) ^ (unsigned long long)(stream * 7919 + 17) };
	splitmix(&r.s);
	return r;
}

static double rnd(Rng *r) { return (double)(splitmix(&r->s) >> 11) * (1.0 / 9007199254740992.0); }
static double range(Rng *r, double lo, double hi) { return lo + (hi - lo) * rnd(r); }
static float frange(Rng *r, double lo, double hi) { return (float)range(r, lo, hi); }
static double sgn(Rng *r) { return rnd(r) < 0.5 ? -1.0 : 1.0; }

static void hsv(double h, double s, double v, float out[3])
{
	h = (h - floor(h)) * 6.0;
	int i = (int)h;
	double f = h - i, p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
	double c[6][3] = { {v,t,p}, {q,v,p}, {p,v,t}, {p,q,v}, {t,p,v}, {v,p,q} };
	for (int k = 0; k < 3; k++)
		out[k] = (float)c[i % 6][k];
}

static void mix3(const float a[3], const float b[3], double t, float out[3])
{
	for (int k = 0; k < 3; k++)
		out[k] = (float)(a[k] + (b[k] - a[k]) * t);
}

static double clamp01(double x)
{
	return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
}

static double smoothstep(double e0, double e1, double x)
{
	double t = clamp01((x - e0) / (e1 - e0));
	return t * t * (3.0 - 2.0 * t);
}

/* ---- Generation ---- */

static void addStorm(GasGiantStyle *s, double latDeg, double lon, double size, double aspect,
                     double spin, const float color[3], double opacity)
{
	if (s->stormCount >= GASGIANT_MAX_STORMS)
		return;
	GasGiantStorm *t = &s->storms[s->stormCount++];
	t->lat = (float)(latDeg * M_PI / 180.0);
	t->lon = (float)lon;
	t->size = (float)size;
	t->aspect = (float)aspect;
	t->spin = (float)spin;
	memcpy(t->color, color, sizeof t->color);
	t->opacity = (float)opacity;
}

/* Anticyclones turn the opposite way in each hemisphere. */
static double spinFor(Rng *r, double latDeg, double lo, double hi)
{
	return (latDeg >= 0.0 ? -1.0 : 1.0) * range(r, lo, hi);
}

static void highBand(GasGiantStyle *s, int i, double latDeg, double widthDeg, double strength)
{
	s->highBand[i][0] = (float)latDeg;
	s->highBand[i][1] = (float)widthDeg;
	s->highBand[i][2] = (float)strength;
}

static void generateStyle(GasGiantStyle *s, unsigned int seed, double aAU, int family)
{
	memset(s, 0, sizeof *s);   /* zero padding too: the rebuild check hashes the bytes */
	s->seed = seed;
	Rng r = rngFor(seed, 1);

	if (family < 0) {
		double t = (log(fmax(aAU, 1e-3)) - log(GAS_ICE_AU_INNER)) / (log(GAS_ICE_AU_OUTER) - log(GAS_ICE_AU_INNER));
		double iceP = GAS_ICE_P_OUTER * clamp01(t);
		double u = rnd(&r);
		if (u < iceP)
			family = GASGIANT_ICE;
		else if ((u = rnd(&r)) < 0.1)
			family = GASGIANT_EXOTIC;
		else if (u < 0.4)
			family = GASGIANT_SATURNIAN;
		else
			family = GASGIANT_JOVIAN;
	}
	s->family = family;

	/* Palettes lean warm and soft (painterly): cream, ochre, rust and umber;
	   ice giants desaturated toward teal-grey. */
	double h;
	switch (family) {
	case GASGIANT_SATURNIAN:
		h = range(&r, 0.10, 0.13);
		hsv(h, range(&r, 0.18, 0.30), range(&r, 0.90, 0.96), s->zone);
		hsv(h - range(&r, 0.02, 0.035), range(&r, 0.30, 0.45), range(&r, 0.68, 0.80), s->belt);
		hsv(rnd(&r) < 0.3 ? range(&r, 0.52, 0.58) : h, range(&r, 0.15, 0.25), range(&r, 0.62, 0.72), s->pole);
		hsv(h + 0.01, 0.1, 0.98, s->accent);
		hsv(h, range(&r, 0.22, 0.32), range(&r, 0.85, 0.92), s->hazeColor);
		s->bandCount = 6 + (int)(rnd(&r) * 5.0);
		s->bandJitter = frange(&r, 0.3, 0.7);
		s->contrast = frange(&r, 0.3, 0.55);
		s->edgeSharpness = frange(&r, 0.15, 0.45);
		s->fineBands = frange(&r, 0.2, 0.45);
		s->turbulence = frange(&r, 0.25, 0.5);
		s->streak = frange(&r, 4.0, 7.0);
		s->detail = frange(&r, 0.15, 0.3);
		s->relief = frange(&r, 0.2, 0.4);
		s->hazeTau = frange(&r, 0.25, 0.45);
		s->wisps = 0.25f;
		s->poleLat = frange(&r, 62.0, 74.0);
		break;
	case GASGIANT_ICE:
		h = range(&r, 0.52, 0.58);
		hsv(h - 0.02, range(&r, 0.10, 0.20), range(&r, 0.84, 0.92), s->zone);
		hsv(h + range(&r, 0.01, 0.04), range(&r, 0.25, 0.38), range(&r, 0.52, 0.66), s->belt);
		hsv(h + 0.02, range(&r, 0.18, 0.28), range(&r, 0.62, 0.72), s->pole);
		hsv(h + 0.05, range(&r, 0.40, 0.55), range(&r, 0.24, 0.32), s->accent);
		hsv(h - 0.03, range(&r, 0.10, 0.18), range(&r, 0.82, 0.9), s->hazeColor);
		s->bandCount = 3 + (int)(rnd(&r) * 4.0);
		s->bandJitter = frange(&r, 0.3, 0.8);
		s->contrast = frange(&r, 0.45, 0.75);
		s->edgeSharpness = frange(&r, 0.1, 0.35);
		s->fineBands = frange(&r, 0.1, 0.3);
		s->turbulence = frange(&r, 0.2, 0.45);
		s->streak = frange(&r, 2.5, 4.0);
		s->detail = frange(&r, 0.06, 0.15);
		s->relief = frange(&r, 0.25, 0.45);
		s->hazeTau = frange(&r, 0.18, 0.35);
		s->wisps = 0.25f;
		s->poleLat = frange(&r, 60.0, 72.0);
		break;
	case GASGIANT_EXOTIC: {
		h = rnd(&r) < 0.7 ? range(&r, -0.08, 0.17) : rnd(&r);
		double s0 = range(&r, 0.10, 0.25);
		hsv(h, s0 * 0.6, range(&r, 0.88, 0.96), s->zone);
		hsv(h + range(&r, -0.04, 0.04), fmin(s0 * 1.6, 1.0), range(&r, 0.50, 0.68), s->belt);
		hsv(h, s0, range(&r, 0.55, 0.65), s->pole);
		hsv(h + sgn(&r) * 0.08, fmin(s0 * 2.0, 1.0), 0.7, s->accent);
		hsv(h, s0 * 0.8, range(&r, 0.85, 0.92), s->hazeColor);
		s->bandCount = 6 + (int)(rnd(&r) * 7.0);
		s->bandJitter = frange(&r, 0.4, 0.9);
		s->contrast = frange(&r, 0.55, 0.9);
		s->edgeSharpness = frange(&r, 0.25, 0.7);
		s->fineBands = frange(&r, 0.15, 0.4);
		s->turbulence = frange(&r, 0.5, 0.9);
		s->streak = frange(&r, 3.0, 6.0);
		s->detail = frange(&r, 0.2, 0.45);
		s->relief = frange(&r, 0.3, 0.6);
		s->hazeTau = frange(&r, 0.1, 0.3);
		s->wisps = 0.3f;
		s->poleLat = frange(&r, 58.0, 70.0);
		break;
	}
	default:   /* GASGIANT_JOVIAN */
		h = range(&r, 0.065, 0.095);
		hsv(h + range(&r, 0.0, 0.015), range(&r, 0.12, 0.22), range(&r, 0.90, 0.96), s->zone);
		hsv(h - range(&r, 0.025, 0.05), range(&r, 0.50, 0.70), range(&r, 0.42, 0.58), s->belt);
		hsv(h - 0.01, range(&r, 0.22, 0.34), range(&r, 0.45, 0.56), s->pole);
		hsv(range(&r, 0.0, 0.03), range(&r, 0.58, 0.75), range(&r, 0.66, 0.78), s->accent);
		hsv(h + 0.01, range(&r, 0.20, 0.30), range(&r, 0.86, 0.92), s->hazeColor);
		s->bandCount = 14 + (int)(rnd(&r) * 9.0);
		s->bandJitter = frange(&r, 0.5, 0.9);
		s->contrast = frange(&r, 0.75, 1.0);
		s->edgeSharpness = frange(&r, 0.45, 0.8);
		s->fineBands = frange(&r, 0.4, 0.8);
		s->turbulence = frange(&r, 0.7, 1.0);
		s->streak = frange(&r, 3.0, 5.0);
		s->detail = frange(&r, 0.3, 0.5);
		s->relief = frange(&r, 0.4, 0.7);
		s->hazeTau = frange(&r, 0.08, 0.18);
		s->wisps = 0.7f;
		s->poleLat = frange(&r, 58.0, 68.0);
		break;
	}
	s->shellFraction = frange(&r, GAS_SHELL_FRACTION_MIN, GAS_SHELL_FRACTION_MAX);

	/* Storms: their own stream, so palette edits above keep them put. */
	Rng sr = rngFor(seed, 2);
	float c[3];
	switch (family) {
	case GASGIANT_JOVIAN: {
		/* One great red spot, then white ovals. */
		double lat = sgn(&sr) * range(&sr, 15.0, 28.0);
		mix3(s->accent, s->belt, 0.25, c);
		addStorm(s, lat, range(&sr, -M_PI, M_PI), range(&sr, 0.08, 0.13), range(&sr, 1.6, 2.1),
		         spinFor(&sr, lat, 2.5, 3.5), c, 0.85);
		int ovals = 1 + (int)(rnd(&sr) * 4.0);
		for (int i = 0; i < ovals; i++) {
			lat = sgn(&sr) * range(&sr, 20.0, 45.0);
			mix3(s->zone, (float[3]){ 1.0f, 1.0f, 1.0f }, 0.3, c);
			addStorm(s, lat, range(&sr, -M_PI, M_PI), range(&sr, 0.015, 0.035), range(&sr, 1.2, 1.5),
			         spinFor(&sr, lat, 2.0, 3.0), c, 0.65);
		}
		/* Barges: long dark ovals in the belts, slowly turning. */
		int barges = (int)(rnd(&sr) * 3.0);
		for (int i = 0; i < barges; i++) {
			lat = sgn(&sr) * range(&sr, 12.0, 22.0);
			mix3(s->belt, s->accent, 0.3, c);
			for (int k = 0; k < 3; k++)
				c[k] *= 0.72f;
			addStorm(s, lat, range(&sr, -M_PI, M_PI), range(&sr, 0.03, 0.05), range(&sr, 2.5, 3.5),
			         spinFor(&sr, lat, 0.8, 1.4), c, 0.7);
		}
		/* A string of pearls: small white ovals in a row. */
		if (rnd(&sr) < 0.5) {
			lat = -range(&sr, 35.0, 42.0);
			double lon0 = range(&sr, -M_PI, M_PI), gap = range(&sr, 0.12, 0.2);
			mix3(s->zone, (float[3]){ 1.0f, 1.0f, 1.0f }, 0.5, c);
			for (int i = 0; i < 4; i++)
				addStorm(s, lat, lon0 + i * gap, range(&sr, 0.011, 0.015), 1.3,
				         spinFor(&sr, lat, 2.0, 2.5), c, 0.75);
		}
		break;
	}
	case GASGIANT_SATURNIAN: {
		int n = (int)(rnd(&sr) * 3.0);
		for (int i = 0; i < n; i++) {
			double lat = sgn(&sr) * range(&sr, 25.0, 50.0);
			mix3(s->zone, s->accent, 0.6, c);
			addStorm(s, lat, range(&sr, -M_PI, M_PI), range(&sr, 0.02, 0.04), range(&sr, 1.3, 1.8),
			         spinFor(&sr, lat, 1.5, 2.5), c, 0.5);
		}
		break;
	}
	case GASGIANT_ICE: {
		int n = (int)(rnd(&sr) * 3.0);
		for (int i = 0; i < n; i++) {
			double lat = sgn(&sr) * range(&sr, 15.0, 40.0), lon = range(&sr, -M_PI, M_PI);
			double size = range(&sr, 0.04, 0.08);
			addStorm(s, lat, lon, size, range(&sr, 1.4, 1.9), spinFor(&sr, lat, 2.0, 3.0), s->accent, 0.75);
			/* A bright companion cloud on its poleward edge. */
			mix3(s->zone, (float[3]){ 1.0f, 1.0f, 1.0f }, 0.7, c);
			addStorm(s, lat + (lat > 0.0 ? 1.0 : -1.0) * size * 0.8 * 180.0 / M_PI, lon + range(&sr, -0.5, 0.5) * size,
			         size * range(&sr, 0.35, 0.5), range(&sr, 2.0, 3.0), 0.0, c, 0.7);
		}
		break;
	}
	default: {
		int n = (int)(rnd(&sr) * 4.0);
		for (int i = 0; i < n; i++) {
			double lat = sgn(&sr) * range(&sr, 10.0, 45.0);
			addStorm(s, lat, range(&sr, -M_PI, M_PI), range(&sr, 0.02, 0.07), range(&sr, 1.3, 2.0),
			         spinFor(&sr, lat, 2.0, 3.5), s->accent, 0.7);
		}
		break;
	}
	}

	/* Polar cyclones: their own stream too. Jovian poles crowd with them;
	   Saturnian ones have a single vortex; ice giants rarely any. */
	Rng pr = rngFor(seed, 5);
	switch (family) {
	case GASGIANT_JOVIAN:    s->polarCount = 5 + (int)(rnd(&pr) * 4.0); break;
	case GASGIANT_SATURNIAN: s->polarCount = 0; break;
	case GASGIANT_ICE:       s->polarCount = rnd(&pr) < 0.4 ? 0 : -1; break;
	default:                 s->polarCount = -1 + (int)(rnd(&pr) * 8.0); break;
	}
	s->polarLat = frange(&pr, 81.0, 85.0);
	s->polarSize = frange(&pr, 0.045, 0.065);
	s->polarSpin = frange(&pr, 2.0, 3.0);
	/* Darker and a touch cooler than the cap. */
	float cool[3];
	hsv(0.58, 0.15, 0.5, cool);
	for (int k = 0; k < 3; k++)
		c[k] = s->pole[k] * 0.85f;
	mix3(c, cool, 0.3, s->polarColor);

	/* High clouds: their own stream. Jovian giants get rows of plumes
	   either side of the equator; ice giants Neptune-like bright streaks
	   at mid latitudes; Saturnian ones a faint band under their haze. */
	Rng hr = rngFor(seed, 6);
	switch (family) {
	case GASGIANT_JOVIAN: {
		double eq = range(&hr, 5.0, 9.0);
		s->high = frange(&hr, 0.4, 0.6);
		s->highHeight = frange(&hr, 0.003, 0.005);
		s->highCount = 3;
		highBand(s, 0, eq, 3.0, 1.0);
		highBand(s, 1, -eq, 3.0, 0.8);
		highBand(s, 2, sgn(&hr) * range(&hr, 28.0, 45.0), 5.0, 0.5);
		break;
	}
	case GASGIANT_SATURNIAN:
		s->high = frange(&hr, 0.2, 0.35);
		s->highHeight = frange(&hr, 0.003, 0.005);
		s->highCount = 1;
		highBand(s, 0, sgn(&hr) * range(&hr, 20.0, 45.0), 6.0, 1.0);
		break;
	case GASGIANT_ICE:
		s->high = frange(&hr, 0.6, 0.9);
		s->highHeight = frange(&hr, 0.006, 0.01);
		s->highCount = 2 + (int)(rnd(&hr) * 2.0);
		highBand(s, 0, range(&hr, 20.0, 35.0), range(&hr, 5.0, 9.0), 1.0);
		highBand(s, 1, -range(&hr, 20.0, 45.0), range(&hr, 5.0, 9.0), 0.9);
		highBand(s, 2, -range(&hr, 55.0, 70.0), 4.0, 0.6);
		break;
	default:
		s->high = frange(&hr, 0.3, 0.6);
		s->highHeight = frange(&hr, 0.003, 0.008);
		s->highCount = 1 + (int)(rnd(&hr) * 3.0);
		for (int i = 0; i < GASGIANT_MAX_HIGH - 1; i++)
			highBand(s, i, sgn(&hr) * range(&hr, 5.0, 50.0), range(&hr, 3.0, 8.0), range(&hr, 0.5, 1.0));
		break;
	}
	/* Near white, a touch of the zone colour. */
	mix3(s->zone, (float[3]){ 1.0f, 0.99f, 0.96f }, 0.8, s->highColor);

	/* Lightning: its own stream. Jovian belts storm the most, ice giants
	   rarely. */
	Rng lr = rngFor(seed, 8);
	switch (family) {
	case GASGIANT_JOVIAN:    s->lightning = frange(&lr, 1.0, 1.5); break;
	case GASGIANT_SATURNIAN: s->lightning = frange(&lr, 0.4, 0.8); break;
	case GASGIANT_ICE:       s->lightning = frange(&lr, 0.15, 0.35); break;
	default:                 s->lightning = frange(&lr, 0.3, 1.2); break;
	}

	/* Aurora: its own stream. Jupiter's field is ~10 degrees off the spin
	   axis, Saturn's hardly at all; Uranus and Neptune's ~50-60 and well
	   off the centre. */
	Rng ar = rngFor(seed, 7);
	s->auroraLon = frange(&ar, -180.0, 180.0);
	switch (family) {
	case GASGIANT_JOVIAN:
		s->aurora = frange(&ar, 0.9, 1.1);
		s->auroraTilt = frange(&ar, 8.0, 12.0);
		s->auroraShift = frange(&ar, 0.0, 3.0);
		s->auroraColat = frange(&ar, 14.0, 18.0);
		s->auroraWidth = frange(&ar, 2.5, 4.0);
		break;
	case GASGIANT_SATURNIAN:
		s->aurora = frange(&ar, 0.7, 0.9);
		s->auroraTilt = frange(&ar, 0.0, 1.0);
		s->auroraShift = frange(&ar, 0.0, 1.0);
		s->auroraColat = frange(&ar, 14.0, 18.0);
		s->auroraWidth = frange(&ar, 2.0, 3.5);
		break;
	case GASGIANT_ICE:
		s->aurora = frange(&ar, 0.6, 0.8);
		s->auroraTilt = frange(&ar, 45.0, 60.0);
		s->auroraShift = frange(&ar, 8.0, 18.0);
		s->auroraColat = frange(&ar, 15.0, 22.0);
		s->auroraWidth = frange(&ar, 3.0, 5.0);
		break;
	default:
		s->aurora = frange(&ar, 0.6, 1.1);
		s->auroraTilt = frange(&ar, 0.0, 40.0);
		s->auroraShift = frange(&ar, 0.0, 8.0);
		s->auroraColat = frange(&ar, 12.0, 22.0);
		s->auroraWidth = frange(&ar, 2.5, 4.5);
		break;
	}
	/* How often it is active (drawn after the rest, so those stay put). */
	switch (family) {
	case GASGIANT_JOVIAN:    s->auroraActive = frange(&ar, 0.5, 0.7); break;
	case GASGIANT_SATURNIAN: s->auroraActive = frange(&ar, 0.3, 0.5); break;
	case GASGIANT_ICE:       s->auroraActive = frange(&ar, 0.15, 0.35); break;
	default:                 s->auroraActive = frange(&ar, 0.15, 0.6); break;
	}
}

void GasGiant_Generate(GasGiantStyle *s, unsigned int seed, double aAU)
{
	generateStyle(s, seed, aAU, -1);
}

void GasGiant_Free(GasGiantStyle *s)
{
	if (s->profileTex)
		glDeleteTextures(1, &s->profileTex);
	s->profileTex = 0;
	s->profileHash = 0;
}

/* generateStyle, keeping the profile texture (the hash rebuilds it). */
static void restyle(GasGiantStyle *s, unsigned int seed, double aAU, int family)
{
	GLuint profile = s->profileTex;
	generateStyle(s, seed, aAU, family);
	s->profileTex = profile;
}

/* ---- Latitude profile ---- */

static double hash1(unsigned int seed, int i)
{
	Rng r = rngFor(seed, i + 100000);
	return rnd(&r);
}

/* 1D value noise along latitude, deterministic per seed. */
static double noise1(unsigned int seed, double x)
{
	double i = floor(x), f = x - i;
	double u = f * f * (3.0 - 2.0 * f);
	double a = hash1(seed, (int)i), b = hash1(seed, (int)i + 1);
	return a + (b - a) * u;
}

#define MAX_BANDS 64

/* RGBA per texel, pole (south) to pole: RGB band colour (display), A the
   turbulence weight (band edges, belts and the polar caps churn most). */
static void buildProfile(const GasGiantStyle *s, float *out)
{
	int nb = s->bandCount < 1 ? 1 : s->bandCount > MAX_BANDS - 2 ? MAX_BANDS - 2 : s->bandCount;
	/* Bands 1..nb between the caps; 0 and nb + 1 are the caps. */
	double edge[MAX_BANDS + 1];      /* edge[k]: boundary below band k (deg) */
	float col[MAX_BANDS][3];
	double turb[MAX_BANDS], width[MAX_BANDS];
	Rng r = rngFor(s->seed, 3);

	double w[MAX_BANDS], sum = 0.0;
	for (int k = 1; k <= nb; k++) {
		w[k] = fmax(1.0 + s->bandJitter * range(&r, -0.8, 0.8), 0.2);
		sum += w[k];
	}
	double pole = fmin(fmax(s->poleLat, 20.0), 85.0);
	edge[1] = -pole;
	for (int k = 1; k <= nb; k++) {
		width[k] = w[k] / sum * 2.0 * pole;
		edge[k + 1] = edge[k] + width[k];
	}
	width[0] = width[nb + 1] = 90.0 - pole;
	edge[0] = -90.0;

	int zone = rnd(&r) < 0.5;
	for (int k = 1; k <= nb; k++, zone = !zone) {
		double t = zone ? range(&r, 0.72, 1.0) : range(&r, 0.0, 0.3);
		t = 0.5 + (t - 0.5) * s->contrast;
		mix3(s->belt, s->zone, t, col[k]);
		if (!zone && rnd(&r) < 0.3)
			mix3(col[k], s->accent, range(&r, 0.25, 0.55), col[k]);
		double v = range(&r, 0.94, 1.06);
		for (int c = 0; c < 3; c++)
			col[k][c] = (float)fmin(col[k][c] * v, 1.0);
		turb[k] = zone ? range(&r, 0.25, 0.6) : range(&r, 0.6, 1.0);
	}
	memcpy(col[0], s->pole, sizeof col[0]);
	memcpy(col[nb + 1], s->pole, sizeof col[0]);
	/* The caps churn gently: strong warps there read as marbling. */
	turb[0] = turb[nb + 1] = 0.4;

	/* Edge half-widths: soft to sharp, never past half the narrower band
	   (so neighbouring blends do not overlap). */
	double half[MAX_BANDS + 1];
	for (int k = 1; k <= nb + 1; k++)
		half[k] = (0.45 - 0.4 * s->edgeSharpness) * fmin(width[k - 1], width[k]);

	/* Fine streaks: two octaves of value noise in latitude. */
	double fineFreq = (nb + 2) * 3.0 / 180.0;
	unsigned int fseed = s->seed ^ 0x51u;

	for (int i = 0; i < PROFILE_N; i++) {
		double lat = ((i + 0.5) / PROFILE_N - 0.5) * 180.0;
		double rgb[3] = { 0.0, 0.0, 0.0 }, tb = 0.0, edgeAmt = 0.0;
		/* Partition of unity over the bands: above[k] is how far lat is past
		   edge k (smoothstep), and band k weighs above[k] - above[k + 1]. */
		double above[MAX_BANDS + 1];
		above[0] = 1.0;
		for (int k = 1; k <= nb + 1; k++)
			above[k] = smoothstep(edge[k] - half[k], edge[k] + half[k], lat);
		above[nb + 2] = 0.0;
		for (int k = 0; k <= nb + 1; k++) {
			double wk = above[k] - above[k + 1];
			if (wk <= 0.0)
				continue;
			for (int c = 0; c < 3; c++)
				rgb[c] += wk * col[k][c];
			tb += wk * turb[k];
		}
		/* Edge turbulence between bands only: the caps fade in (below). */
		for (int k = 2; k <= nb; k++) {
			double d = (lat - edge[k]) / (0.35 * fmin(width[k - 1], width[k]));
			edgeAmt = fmax(edgeAmt, exp(-d * d));
		}
		/* A wide fade into the cap instead of a hard rim. */
		double cap = smoothstep(pole - 12.0, pole + 3.0, fabs(lat)) * 0.6;
		for (int c = 0; c < 3; c++)
			rgb[c] += (s->pole[c] - rgb[c]) * cap;
		double fine = (noise1(fseed, lat * fineFreq) - 0.5) + 0.5 * (noise1(fseed + 1u, lat * fineFreq * 2.7) - 0.5);
		double v = 1.0 + s->fineBands * 0.16 * fine;
		for (int c = 0; c < 3; c++)
			out[i * 4 + c] = (float)fmin(fmax(rgb[c] * v, 0.0), 1.0);
		out[i * 4 + 3] = (float)clamp01(fmax(0.6 * tb, edgeAmt * (0.5 + 0.5 * tb)));
	}
}

void GasGiant_MeanColor(const GasGiantStyle *s, float out[3])
{
	static float prof[PROFILE_N * 4];
	buildProfile(s, prof);
	double sum[3] = { 0.0, 0.0, 0.0 }, wsum = 0.0;
	for (int i = 0; i < PROFILE_N; i++) {
		double w = cos(((i + 0.5) / PROFILE_N - 0.5) * M_PI);
		for (int c = 0; c < 3; c++)
			sum[c] += w * prof[i * 4 + c];
		wsum += w;
	}
	for (int c = 0; c < 3; c++)
		out[c] = (float)(sum[c] / wsum);
}

/* FNV-1a over every field before the built ones. */
static unsigned int styleHash(const GasGiantStyle *s)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned int h = 2166136261u;
	for (size_t i = 0; i < offsetof(GasGiantStyle, profileTex); i++)
		h = (h ^ p[i]) * 16777619u;
	return h | 1u;   /* never 0: 0 = not built */
}

static void uploadProfile(GasGiantStyle *s)
{
	static float prof[PROFILE_N * 4];
	buildProfile(s, prof);
	if (!s->profileTex) {
		glGenTextures(1, &s->profileTex);
		glBindTexture(GL_TEXTURE_1D, s->profileTex);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	} else {
		glBindTexture(GL_TEXTURE_1D, s->profileTex);
	}
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA16F, PROFILE_N, 0, GL_RGBA, GL_FLOAT, prof);
	glGenerateMipmap(GL_TEXTURE_1D);

	/* The turbulence weight, coarser, for placing the flashes. */
	const int per = PROFILE_N / GASGIANT_TURB_N;
	for (int k = 0; k < GASGIANT_TURB_N; k++) {
		float sum = 0.0f;
		for (int i = 0; i < per; i++)
			sum += prof[(k * per + i) * 4 + 3];
		s->turb[k] = sum / (float)per;
	}
}

/* ---- Aurora activity ---- */

/* How strongly s's aurora shows now (0..1): a slow noise on the scene
   clock (spells of about auroraCycle s) above a threshold set by the
   share of the time it is active, faded over the threshold's edges, and
   some episodes weaker than others. Nothing is stored: every giant has
   its own spells, and bench runs repeat. */
static double auroraActivity(const GasGiantStyle *s)
{
	const GasGiantParams *g = &g_params;
	if (g->auroraAlways)
		return 1.0;
	double p = clamp01(s->auroraActive);
	if (p <= 0.0)
		return 0.0;
	double x = g_time / fmax(g->auroraCycle, 1.0);
	unsigned int seed = s->seed ^ 0xA0A0u;
	double n = 0.75 * noise1(seed, x) + 0.25 * noise1(seed + 7u, x * 2.7 + 13.0);
	/* The noise gathers toward the middle: its share above th is about
	   p when th is set from p this way. */
	double th = 0.5 + 0.7 * (0.5 - p);
	double on = smoothstep(th - 0.06, th + 0.06, n);
	double strength = 0.55 + 0.45 * noise1(seed + 19u, x * 0.6 + 5.0);
	return on * strength;
}

/* ---- Lightning ---- */

/* Each flash slot's timeline is cut into periods; a hash of (seed, slot,
   period) says whether the slot fires in it, when and where. Nothing is
   stored, so every giant in view storms on its own and runs repeat. */
#define FLASH_PERIOD 2.0   /* s, jittered per slot */
#define FLASH_LIFE   1.0   /* s, at most */

typedef struct FlashCell {
	double start, life;    /* within the period, s */
	int strokes;
	double gap[4];         /* each stroke after the cell starts, s */
	double weight[4];
	float dir[3];          /* body-fixed, unit */
} FlashCell;

/* The profile's turbulence weight at latitude lat (rad). */
static double turbAt(const GasGiantStyle *s, double lat)
{
	int k = (int)((lat / M_PI + 0.5) * GASGIANT_TURB_N);
	k = k < 0 ? 0 : k >= GASGIANT_TURB_N ? GASGIANT_TURB_N - 1 : k;
	return s->turb[k];
}

/* Where a cell storms: mostly the churning belts (a candidate latitude
   kept with the turbulence weight squared as its chance), some at the
   storms' edges, some in the polar cyclones' ring. */
static void flashPlace(const GasGiantStyle *s, Rng *r, float dir[3])
{
	double lat = 0.0, lon = range(r, -M_PI, M_PI), u = rnd(r);
	if (u < 0.2 && s->stormCount > 0) {
		const GasGiantStorm *t = &s->storms[(int)(rnd(r) * s->stormCount) % s->stormCount];
		double a = range(r, 0.0, 2.0 * M_PI), d = t->size * range(r, 0.6, 1.2);
		lat = t->lat + sin(a) * d / fmax(t->aspect, 1.0);
		lon = t->lon + cos(a) * d;
	} else if (u < 0.3 && s->polarCount >= 0) {
		lat = sgn(r) * (s->polarLat + range(r, -3.0, 3.0)) * M_PI / 180.0;
	} else {
		for (int tries = 0; tries < 4; tries++) {
			lat = asin(range(r, -1.0, 1.0));
			double w = turbAt(s, lat);
			if (rnd(r) < w * w)
				break;
		}
	}
	double cl = cos(lat);
	dir[0] = (float)(cl * cos(lon));
	dir[1] = (float)(cl * sin(lon));
	dir[2] = (float)sin(lat);
}

/* Slot's cell for period k, if it fires: 2-4 strokes, then a dim
   afterglow. rate: cells per second over the whole giant. */
static int flashCell(const GasGiantStyle *s, int slot, long long k, double period, double rate, FlashCell *c)
{
	Rng r = rngFor(s->seed * 0x9E3779B1u ^ (unsigned int)k, 8000 + slot);
	if (rnd(&r) >= rate * period / GASGIANT_MAX_FLASHES)
		return 0;
	c->life = range(&r, 0.5, FLASH_LIFE);
	c->start = range(&r, 0.0, period - c->life);
	c->strokes = 2 + (int)(rnd(&r) * 3.0);
	double at = 0.0, w = 1.0;
	for (int i = 0; i < c->strokes; i++) {
		c->gap[i] = at;
		c->weight[i] = w;
		at += range(&r, 0.06, 0.22);
		w *= range(&r, 0.5, 0.9);
	}
	flashPlace(s, &r, c->dir);
	return 1;
}

static double flashEnvelope(const FlashCell *c, double age)
{
	if (age < 0.0 || age > c->life)
		return 0.0;
	double e = 0.15 * exp(-age / 0.25);
	for (int i = 0; i < c->strokes; i++)
		if (age >= c->gap[i])
			e += c->weight[i] * exp(-(age - c->gap[i]) / 0.05);
	/* Out by the end of its life. */
	return e * (1.0 - smoothstep(0.8 * c->life, c->life, age));
}

/* This frame's flashes on s: their directions and sizes (radii), and
   brightness. Returns how many. */
static int flashesNow(const GasGiantStyle *s, double radius, float dir[][4], float gain[])
{
	const GasGiantParams *g = &g_params;
	double rate = g->lightning * s->lightning;
	int n = 0;
	if (rate <= 0.0 && !g->flashHold)
		return 0;
	for (int slot = 0; slot < GASGIANT_MAX_FLASHES; slot++) {
		/* Jittered per slot, so the slots don't fire in step. */
		double period = FLASH_PERIOD * (0.8 + 0.4 * hash1(s->seed ^ 0x7Fu, slot));
		double t = g_time + period * hash1(s->seed ^ 0x3Bu, slot);
		long long k = (long long)floor(t / period);
		FlashCell c;
		double e;
		if (g->flashHold) {
			/* Steady, placed by slot, for tuning. */
			Rng r = rngFor(s->seed, 8000 + slot);
			flashPlace(s, &r, c.dir);
			e = 1.0;
		} else {
			if (!flashCell(s, slot, k, period, rate, &c))
				continue;
			e = flashEnvelope(&c, t - k * period - c.start);
			if (e <= 1e-3)
				continue;
		}
		dir[n][0] = c.dir[0];
		dir[n][1] = c.dir[1];
		dir[n][2] = c.dir[2];
		dir[n][3] = (float)(g->flashSize / radius);
		gain[n] = (float)(e * g->flash);
		n++;
	}
	return n;
}

/* ---- Noise texture ---- */

static float lattice[NOISE_PERIOD][NOISE_PERIOD][NOISE_PERIOD][4];

static float quintic(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* Four independent periodic value noises (quintic, as gasgiant.glsl's
   octaves expect), sampled at texel centres. */
void GasGiant_Init(void)
{
	Rng r = rngFor(0x6a5u, 0);
	for (int z = 0; z < NOISE_PERIOD; z++)
		for (int y = 0; y < NOISE_PERIOD; y++)
			for (int x = 0; x < NOISE_PERIOD; x++)
				for (int c = 0; c < 4; c++)
					lattice[z][y][x][c] = (float)rnd(&r);

	unsigned char *texels = malloc((size_t)NOISE_N * NOISE_N * NOISE_N * 4);
	const float cell = (float)NOISE_PERIOD / NOISE_N;
	int z;
	#pragma omp parallel for
	for (z = 0; z < NOISE_N; z++)
		for (int y = 0; y < NOISE_N; y++)
			for (int x = 0; x < NOISE_N; x++) {
				float p[3] = { (x + 0.5f) * cell, (y + 0.5f) * cell, (z + 0.5f) * cell };
				int i0[3], i1[3];
				float u[3];
				for (int k = 0; k < 3; k++) {
					float f = floorf(p[k]);
					i0[k] = (int)f % NOISE_PERIOD;
					i1[k] = (i0[k] + 1) % NOISE_PERIOD;
					u[k] = quintic(p[k] - f);
				}
				unsigned char *out = texels + (((size_t)z * NOISE_N + y) * NOISE_N + x) * 4;
				for (int c = 0; c < 4; c++) {
					float v[8];
					for (int corner = 0; corner < 8; corner++) {
						int ix = corner & 1 ? i1[0] : i0[0];
						int iy = corner & 2 ? i1[1] : i0[1];
						int iz = corner & 4 ? i1[2] : i0[2];
						v[corner] = lattice[iz][iy][ix][c];
					}
					float x00 = v[0] + (v[1] - v[0]) * u[0], x10 = v[2] + (v[3] - v[2]) * u[0];
					float x01 = v[4] + (v[5] - v[4]) * u[0], x11 = v[6] + (v[7] - v[6]) * u[0];
					float y0 = x00 + (x10 - x00) * u[1], y1 = x01 + (x11 - x01) * u[1];
					float n = y0 + (y1 - y0) * u[2];
					out[c] = (unsigned char)(fminf(fmaxf(n, 0.0f), 1.0f) * 255.0f + 0.5f);
				}
			}

	glGenTextures(1, &noiseTex);
	glBindTexture(GL_TEXTURE_3D, noiseTex);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, NOISE_N, NOISE_N, NOISE_N, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_3D, 0);
	free(texels);
}

/* ---- Stand-in binding ---- */

void GasGiant_AttachStandIn(GLuint shader)
{
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "gasProfile"), GASGIANT_PROFILE_UNIT);
	glUniform1i(glGetUniformLocation(shader, "gasNoiseTex"), GASGIANT_NOISE_UNIT);
	glUniform1i(glGetUniformLocation(shader, "gasOn"), 0);
}

static void uniform3(GLuint shader, const char *name, const float v[3])
{
	glUniform3f(glGetUniformLocation(shader, name), v[0], v[1], v[2]);
}

void GasGiant_BindStandIn(GLuint shader, GasGiantStyle *s, double radius, double altitude)
{
	glUniform1i(glGetUniformLocation(shader, "gasOn"), s != NULL);
	if (!s)
		return;
	double H = GasInterior_ScaleHeight(s, radius);
	glUniform1f(glGetUniformLocation(shader, "gasInside"), 1.0f - GasInterior_StandInHaze(H, altitude));
	/* The glow beyond the limb assumes the camera is outside the shell:
	   gone by the time the interior pass starts. */
	glUniform1f(glGetUniformLocation(shader, "gasGlowFade"),
	            (float)smoothstep(GAS_INTERIOR_FADE_HI_H * H, GAS_HAZE_TOP_H * H, altitude));
	glUniform1f(glGetUniformLocation(shader, "gasDeep"), GasInterior_VolumeFade(H, altitude));
	glUniform1f(glGetUniformLocation(shader, "gasDeepReach"), (float)GasInterior_CloudReach(H, radius, altitude));
	glUniform1i(glGetUniformLocation(shader, "gasOctaves"),
	            altitude < GAS_CLOSE_DETAIL_ALT_RADII * radius ? 8 : 5);

	unsigned int h = styleHash(s);
	if (!s->profileTex || h != s->profileHash) {
		glActiveTexture(GL_TEXTURE0 + GASGIANT_PROFILE_UNIT);
		uploadProfile(s);
		s->profileHash = h;
	}
	glActiveTexture(GL_TEXTURE0 + GASGIANT_PROFILE_UNIT);
	glBindTexture(GL_TEXTURE_1D, s->profileTex);
	glActiveTexture(GL_TEXTURE0 + GASGIANT_NOISE_UNIT);
	glBindTexture(GL_TEXTURE_3D, noiseTex);
	glActiveTexture(GL_TEXTURE0);

	/* Noise: north-south frequency per radius follows the bands; the seed
	   picks an offset into the noise. */
	glUniform1f(glGetUniformLocation(shader, "gasFreq"), 2.0f + 0.6f * (float)s->bandCount);
	glUniform3f(glGetUniformLocation(shader, "gasSeedOffset"),
	            (float)(s->seed % 89u) * 3.17f, (float)(s->seed % 97u) * 2.71f, (float)(s->seed % 83u) * 1.93f);
	glUniform1f(glGetUniformLocation(shader, "gasStreak"), s->streak);
	glUniform1f(glGetUniformLocation(shader, "gasTurbulence"), s->turbulence);
	glUniform1f(glGetUniformLocation(shader, "gasDetail"), s->detail);
	glUniform1f(glGetUniformLocation(shader, "gasWisps"), s->wisps);
	glUniform1i(glGetUniformLocation(shader, "gasPolarCount"),
	            s->polarCount < -1 ? -1 : s->polarCount > GASGIANT_MAX_POLAR ? GASGIANT_MAX_POLAR : s->polarCount);
	glUniform4f(glGetUniformLocation(shader, "gasPolar"), (float)((90.0 - s->polarLat) * M_PI / 180.0),
	            s->polarSize, s->polarSpin, (float)(s->seed % 360u) * (float)(M_PI / 180.0));
	uniform3(shader, "gasPolarColor", s->polarColor);
	float high[GASGIANT_MAX_HIGH][3];
	int nh = s->highCount < 0 ? 0 : s->highCount > GASGIANT_MAX_HIGH ? GASGIANT_MAX_HIGH : s->highCount;
	for (int i = 0; i < GASGIANT_MAX_HIGH; i++) {
		high[i][0] = s->highBand[i][0] * (float)(M_PI / 180.0);
		high[i][1] = fmaxf(s->highBand[i][1], 0.1f) * (float)(M_PI / 180.0);
		high[i][2] = s->highBand[i][2];
	}
	glUniform1f(glGetUniformLocation(shader, "gasHigh"), s->high);
	glUniform1f(glGetUniformLocation(shader, "gasHighHeight"), s->highHeight);
	uniform3(shader, "gasHighColor", s->highColor);
	glUniform1i(glGetUniformLocation(shader, "gasHighCount"), nh);
	glUniform3fv(glGetUniformLocation(shader, "gasHighBand"), GASGIANT_MAX_HIGH, &high[0][0]);
	glUniform1f(glGetUniformLocation(shader, "gasRelief"), s->relief);
	glUniform1f(glGetUniformLocation(shader, "gasHazeTau"), s->hazeTau);
	uniform3(shader, "gasHazeColor", s->hazeColor);

	float storm[GASGIANT_MAX_STORMS][4], look[GASGIANT_MAX_STORMS][4], spin[GASGIANT_MAX_STORMS];
	int n = s->stormCount < 0 ? 0 : s->stormCount > GASGIANT_MAX_STORMS ? GASGIANT_MAX_STORMS : s->stormCount;
	for (int i = 0; i < GASGIANT_MAX_STORMS; i++) {
		const GasGiantStorm *t = &s->storms[i];
		storm[i][0] = t->lat;
		storm[i][1] = t->lon;
		storm[i][2] = t->size;
		storm[i][3] = t->aspect;
		look[i][0] = t->color[0];
		look[i][1] = t->color[1];
		look[i][2] = t->color[2];
		look[i][3] = t->opacity;
		spin[i] = t->spin;
	}
	glUniform1i(glGetUniformLocation(shader, "gasStormCount"), n);
	glUniform4fv(glGetUniformLocation(shader, "gasStorm"), GASGIANT_MAX_STORMS, &storm[0][0]);
	glUniform4fv(glGetUniformLocation(shader, "gasStormColor"), GASGIANT_MAX_STORMS, &look[0][0]);
	glUniform1fv(glGetUniformLocation(shader, "gasStormSpin"), GASGIANT_MAX_STORMS, spin);

	float flashDir[GASGIANT_MAX_FLASHES][4] = { { 0.0f } }, flashGain[GASGIANT_MAX_FLASHES] = { 0.0f };
	int nf = flashesNow(s, radius, flashDir, flashGain);
	glUniform1i(glGetUniformLocation(shader, "gasFlashCount"), nf);
	glUniform4fv(glGetUniformLocation(shader, "gasFlash"), GASGIANT_MAX_FLASHES, &flashDir[0][0]);
	glUniform1fv(glGetUniformLocation(shader, "gasFlashGain"), GASGIANT_MAX_FLASHES, flashGain);

	const GasGiantParams *g = &g_params;
	/* Aurora: the magnetic poles in body-fixed axes (the south one on the
	   far meridian), and the shimmer's phase (kept small for floats). */
	{
		double lon = s->auroraLon * M_PI / 180.0;
		double cn = (s->auroraTilt + s->auroraShift) * M_PI / 180.0;
		double cs = (s->auroraTilt - s->auroraShift) * M_PI / 180.0;
		float poles[2][3] = {
			{ (float)(sin(cn) * cos(lon)), (float)(sin(cn) * sin(lon)), (float)cos(cn) },
			{ (float)(-sin(cs) * cos(lon)), (float)(-sin(cs) * sin(lon)), (float)-cos(cs) },
		};
		glUniform1f(glGetUniformLocation(shader, "gasAurora"), (float)(s->aurora * g->aurora * auroraActivity(s)));
		glUniform3fv(glGetUniformLocation(shader, "gasAuroraPole"), 2, &poles[0][0]);
		glUniform2f(glGetUniformLocation(shader, "gasAuroraOval"),
		            (float)(s->auroraColat * M_PI / 180.0), (float)(fmax(s->auroraWidth, 0.1) * M_PI / 180.0));
		glUniform1f(glGetUniformLocation(shader, "gasAuroraDay"), g->auroraDay);
		glUniform1f(glGetUniformLocation(shader, "gasAuroraPhase"), (float)fmod(g_time * g->auroraSpeed, 4096.0));
		glUniform1f(glGetUniformLocation(shader, "gasAuroraGreen"), g->auroraGreen);
		glUniform1f(glGetUniformLocation(shader, "gasAuroraHeight"), (float)GAS_AURORA_HEIGHT_RADII);
		glUniform1f(glGetUniformLocation(shader, "gasAuroraLean"), g->auroraLean);
		uniform3(shader, "gasAuroraLow", g->auroraLow);
		uniform3(shader, "gasAuroraMid", g->auroraMid);
		uniform3(shader, "gasAuroraHigh", g->auroraHigh);
	}
	glUniform1f(glGetUniformLocation(shader, "gasLimbK"), g->limbK);
	glUniform1f(glGetUniformLocation(shader, "gasWrap"), g->wrap);
	uniform3(shader, "gasTerminator", g->terminator);
	glUniform1f(glGetUniformLocation(shader, "gasAmbient"), g->ambient);
	glUniform1f(glGetUniformLocation(shader, "gasRim"), g->rim);
	glUniform1f(glGetUniformLocation(shader, "gasGlow"), g->glow);
	glUniform1f(glGetUniformLocation(shader, "gasExposure"), g->exposure);
}

/* ---- Debug panel (a tab of the engine/debug/debug_gui.c window) ---- */

/* name | colour swatch (opens a picker) | values. */
static void colorRow(struct nk_context *ctx, const char *name, float c[3])
{
	DebugGUI_RowLayout(ctx);
	nk_label(ctx, name, NK_TEXT_LEFT);
	struct nk_colorf f = { c[0], c[1], c[2], 1.0f };
	if (nk_combo_begin_color(ctx, nk_rgb_cf(f), nk_vec2(nk_widget_width(ctx), 260.0f))) {
		nk_layout_row_dynamic(ctx, 160.0f, 1);
		f = nk_color_picker(ctx, f, NK_RGB);
		nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
		f.r = nk_propertyf(ctx, "#R:", 0.0f, f.r, 1.0f, 0.01f, 0.005f);
		f.g = nk_propertyf(ctx, "#G:", 0.0f, f.g, 1.0f, 0.01f, 0.005f);
		f.b = nk_propertyf(ctx, "#B:", 0.0f, f.b, 1.0f, 0.01f, 0.005f);
		nk_combo_end(ctx);
		c[0] = f.r;
		c[1] = f.g;
		c[2] = f.b;
	}
	nk_labelf(ctx, NK_TEXT_RIGHT, "%.2f %.2f %.2f", c[0], c[1], c[2]);
}

void GasGiant_DebugPanel(struct nk_context *ctx)
{
	DebugGUI_Group(ctx, "Focus a giant (Tab / Shift+Tab)");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 3);
	int any = 0;
	for (int i = 0; i < BODY_COUNT; i++) {
		if (SolarSystem_Info(i)->type != PLANET_GAS)
			continue;
		any = 1;
		int on = (i == Bodies_Params()->focusBody);
		if (nk_select_label(ctx, Bodies_Get(i)->name, NK_TEXT_CENTERED, on) && !on)
			Bodies_Params()->focusBody = i;
	}
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	if (!any)
		nk_label(ctx, "No gas giants in this system.", NK_TEXT_LEFT);

	int id = Bodies_Params()->focusBody;
	PlanetInfo *pi = SolarSystem_Info(id);
	Body *b = Bodies_Get(id);
	if (pi->type == PLANET_GAS) {
		GasGiantStyle *s = &pi->gas;
		double aAU = b->orbit.a / AU_M;
		DebugGUI_Group(ctx, b->name);

		int family = s->family;
		DebugGUI_RowLayout(ctx);
		nk_label(ctx, "Family", NK_TEXT_LEFT);
		nk_slider_int(ctx, 0, &family, GASGIANT_FAMILY_COUNT - 1, 1);
		nk_label(ctx, GasGiant_FamilyName(family), NK_TEXT_RIGHT);
		int seed = (int)(s->seed & 0x7fffffff), newSeed = seed;
		nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 2);
		nk_property_int(ctx, "Style seed", 0, &newSeed, INT_MAX, 1, 1.0f);
		if (nk_button_label(ctx, "Re-roll"))
			newSeed = seed + 1;
		if (family != s->family || newSeed != seed)
			restyle(s, (unsigned int)newSeed, aAU, family != s->family ? family : -1);

		DebugGUI_Group(ctx, "Bands");
		DebugGUI_SliderInt(ctx, "Bands", &s->bandCount, 1, 40);
		DebugGUI_Slider(ctx, "Width jitter", &s->bandJitter, 0.0f, 1.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Polar cap (deg)", &s->poleLat, 30.0f, 85.0f, 0.5f, "%.1f");
		DebugGUI_Slider(ctx, "Contrast", &s->contrast, 0.0f, 1.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Edge sharpness", &s->edgeSharpness, 0.0f, 1.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Fine streaks", &s->fineBands, 0.0f, 1.0f, 0.01f, "%.2f");
		colorRow(ctx, "Zone", s->zone);
		colorRow(ctx, "Belt", s->belt);
		colorRow(ctx, "Pole", s->pole);
		colorRow(ctx, "Accent", s->accent);

		DebugGUI_Group(ctx, "Clouds");
		DebugGUI_Slider(ctx, "Turbulence", &s->turbulence, 0.0f, 2.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Streak", &s->streak, 1.0f, 10.0f, 0.1f, "%.1f");
		DebugGUI_Slider(ctx, "Detail", &s->detail, 0.0f, 1.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Relief", &s->relief, 0.0f, 1.5f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Wisps", &s->wisps, 0.0f, 1.5f, 0.01f, "%.2f");
		DebugGUI_SliderInt(ctx, "Storms", &s->stormCount, 0, GASGIANT_MAX_STORMS);

		DebugGUI_Group(ctx, "Polar cyclones");
		DebugGUI_SliderInt(ctx, "Ring (-1 none)", &s->polarCount, -1, GASGIANT_MAX_POLAR);
		DebugGUI_Slider(ctx, "Ring latitude", &s->polarLat, 70.0f, 89.0f, 0.1f, "%.1f");
		DebugGUI_Slider(ctx, "Size (rad)", &s->polarSize, 0.01f, 0.12f, 0.001f, "%.3f");
		DebugGUI_Slider(ctx, "Spin", &s->polarSpin, -5.0f, 5.0f, 0.05f, "%.2f");
		colorRow(ctx, "Core colour", s->polarColor);

		DebugGUI_Group(ctx, "High clouds");
		DebugGUI_Slider(ctx, "Amount", &s->high, 0.0f, 1.5f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Height (radii)", &s->highHeight, 0.0f, 0.03f, 0.0005f, "%.4f");
		colorRow(ctx, "Colour", s->highColor);
		DebugGUI_SliderInt(ctx, "Bands", &s->highCount, 0, GASGIANT_MAX_HIGH);
		for (int i = 0; i < s->highCount && i < GASGIANT_MAX_HIGH; i++) {
			char name[32];
			snprintf(name, sizeof name, "Band %d lat", i + 1);
			DebugGUI_Slider(ctx, name, &s->highBand[i][0], -85.0f, 85.0f, 0.5f, "%.1f");
			snprintf(name, sizeof name, "Band %d width", i + 1);
			DebugGUI_Slider(ctx, name, &s->highBand[i][1], 0.5f, 30.0f, 0.1f, "%.1f");
			snprintf(name, sizeof name, "Band %d strength", i + 1);
			DebugGUI_Slider(ctx, name, &s->highBand[i][2], 0.0f, 1.5f, 0.01f, "%.2f");
		}

		DebugGUI_Group(ctx, "Haze");
		DebugGUI_Slider(ctx, "Haze tau", &s->hazeTau, 0.0f, 1.5f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Shell (radii)", &s->shellFraction, 0.0f, 0.1f, 0.001f, "%.3f");
		colorRow(ctx, "Haze colour", s->hazeColor);

		DebugGUI_Group(ctx, "Night side");
		DebugGUI_Slider(ctx, "Lightning", &s->lightning, 0.0f, 3.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Aurora", &s->aurora, 0.0f, 3.0f, 0.01f, "%.2f");
		DebugGUI_Slider(ctx, "Magnetic tilt (deg)", &s->auroraTilt, 0.0f, 90.0f, 0.5f, "%.1f");
		DebugGUI_Slider(ctx, "Tilt toward (deg)", &s->auroraLon, -180.0f, 180.0f, 1.0f, "%.0f");
		DebugGUI_Slider(ctx, "Off-centre (deg)", &s->auroraShift, -30.0f, 30.0f, 0.5f, "%.1f");
		DebugGUI_Slider(ctx, "Oval radius (deg)", &s->auroraColat, 3.0f, 45.0f, 0.1f, "%.1f");
		DebugGUI_Slider(ctx, "Oval width (deg)", &s->auroraWidth, 0.3f, 15.0f, 0.1f, "%.1f");
		DebugGUI_Slider(ctx, "Aurora active (share)", &s->auroraActive, 0.0f, 1.0f, 0.01f, "%.2f");
		DebugGUI_RowLayout(ctx);
		nk_label(ctx, "Aurora now", NK_TEXT_LEFT);
		nk_label(ctx, "", NK_TEXT_LEFT);
		nk_labelf(ctx, NK_TEXT_RIGHT, "%.2f", auroraActivity(s));

		/* The dot colour follows the bands. */
		GasGiant_MeanColor(s, b->color);
	} else {
		nk_label(ctx, "Focus a gas giant to edit its style.", NK_TEXT_LEFT);
	}

	GasGiantParams *g = &g_params;
	DebugGUI_Group(ctx, "Look (all giants)");
	DebugGUI_Slider(ctx, "Limb darkening k", &g->limbK, 0.5f, 2.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Wrap", &g->wrap, 0.0f, 0.6f, 0.01f, "%.2f");
	colorRow(ctx, "Terminator tint", g->terminator);
	DebugGUI_Slider(ctx, "Night ambient", &g->ambient, 0.0f, 0.2f, 0.001f, "%.3f");
	DebugGUI_Slider(ctx, "Haze rim", &g->rim, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Limb glow", &g->glow, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Exposure", &g->exposure, 0.3f, 3.0f, 0.01f, "%.2f");

	DebugGUI_Group(ctx, "Night side (all giants)");
	DebugGUI_Slider(ctx, "Lightning (per s)", &g->lightning, 0.0f, 20.0f, 0.1f, "%.1f");
	DebugGUI_Slider(ctx, "Flash brightness", &g->flash, 0.0f, 30.0f, 0.1f, "%.1f");
	float km = g->flashSize * 1e-3f;
	DebugGUI_Slider(ctx, "Flash size (km)", &km, 20.0f, 2000.0f, 5.0f, "%.0f");
	g->flashSize = km * 1e3f;
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Hold flashes (steady, for tuning)", &g->flashHold);
	DebugGUI_Slider(ctx, "Aurora brightness", &g->aurora, 0.0f, 10.0f, 0.05f, "%.2f");
	DebugGUI_Slider(ctx, "Aurora by day", &g->auroraDay, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Aurora shimmer", &g->auroraSpeed, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Aurora green", &g->auroraGreen, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Curtain lean (widths)", &g->auroraLean, 0.0f, 5.0f, 0.05f, "%.2f");
	DebugGUI_Slider(ctx, "Aurora spells (s)", &g->auroraCycle, 5.0f, 600.0f, 1.0f, "%.0f");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Aurora always on (for tuning)", &g->auroraAlways);
	colorRow(ctx, "Aurora low (edge)", g->auroraLow);
	colorRow(ctx, "Aurora mid", g->auroraMid);
	colorRow(ctx, "Aurora high", g->auroraHigh);

	GasInterior_DebugPanel(ctx);
}
