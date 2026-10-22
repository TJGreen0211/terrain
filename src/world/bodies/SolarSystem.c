#include "world/bodies/SolarSystem.h"
#include "world/rings/Rings.h"
#include "world/bodies/Planets.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

PlanetInfo g_planetInfo[BODY_COUNT];
unsigned int g_systemSeed = SYSTEM_SEED_DEFAULT;

static int activeBody = BODY_EARTH;

/* The background bake: one at a time. bakeBody -1 = its planet was
   re-generated since; the result is dropped. */
static TerrainBakeJob *bakeJob;
static int bakeBody = -1;

/* Earth's start-up style and bake settings: the templates generated
   planets vary from (captured before any debug-panel edit). */
static PlanetStyle baseStyle;
static TerrainBakeSettings baseBake;

/* Look constants for generation (chances, palettes); scale-dependent ranges
   are in WorldConfig.h. */
#define ROCKY_RING_CHANCE   0.15
#define DWARF_RING_CHANCE   0.08
#define GAS_RING_CHANCE     0.5
#define DWARF_ATMO_CHANCE   0.1
/* Screen radius (px) at which a terrain planet becomes the active one (and
   starts its background bake, the first time; it switches once baked).
   Below Planets.c's full-detail size so the switch usually happens before
   it is needed. */
#define ACTIVATE_PX         2.0

/* ---- Random numbers: one stream per body, from the system seed ---- */

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
static double logRange(Rng *r, double lo, double hi) { return exp(range(r, log(lo), log(hi))); }
static float frange(Rng *r, double lo, double hi) { return (float)range(r, lo, hi); }

static void hsv(double h, double s, double v, float out[3])
{
	h = (h - floor(h)) * 6.0;
	int i = (int)h;
	double f = h - i, p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
	double c[6][3] = { {v,t,p}, {q,v,p}, {p,v,t}, {p,q,v}, {t,p,v}, {v,p,q} };
	for (int k = 0; k < 3; k++)
		out[k] = (float)c[i % 6][k];
}

/* ---- Generation ---- */

/* Outward order of the orbit slots (Earth's is fixed at 1 AU). */
static const int majorSlots[] = { BODY_MERCURY, BODY_VENUS, BODY_EARTH, BODY_MARS,
                                  BODY_JUPITER, BODY_SATURN, BODY_URANUS, BODY_NEPTUNE };
#define MAJOR_COUNT ((int)(sizeof majorSlots / sizeof majorSlots[0]))
#define EARTH_SLOT 2
static const int kuiperSlots[] = { BODY_PLUTO, BODY_HAUMEA, BODY_MAKEMAKE };

static double gasChance(double aAU)
{
	double t = (log(aAU) - log(GAS_AU_INNER)) / (log(GAS_AU_OUTER) - log(GAS_AU_INNER));
	t = fmin(fmax(t, 0.0), 1.0);
	return GAS_P_INNER + (GAS_P_OUTER - GAS_P_INNER) * t;
}

static void randomOrbit(Rng *r, OrbitElements *o, double aAU, double eMax, double iMax)
{
	o->a = aAU * AU_M;
	o->e = range(r, 0.0, eMax);
	o->i = range(r, 0.0, iMax);
	o->node = range(r, 0.0, 360.0);
	o->peri = range(r, 0.0, 360.0);
	o->L = range(r, 0.0, 360.0);
	o->period = 0.0;
}

static double randomTilt(Rng *r)
{
	double u = rnd(r);
	if (u < 0.80) return range(r, 0.0, 30.0);
	if (u < 0.95) return range(r, 30.0, 90.0);
	return range(r, 90.0, 180.0);   /* retrograde spin */
}

/* Rocky / dwarf surface, atmosphere and terrain recipe. temp: 1 = Earth's
   distance, higher = hotter (1/sqrt(a)). */
static void generateRocky(Rng *r, PlanetInfo *p, const Body *b, double temp, int dwarf)
{
	double sizeE = b->radius / EARTH_RADIUS_M;

	/* Atmosphere: likelier on bigger planets. */
	double atmoChance = dwarf ? DWARF_ATMO_CHANCE : fmin(fmax((sizeE - 0.25) / 0.5, 0.0), 1.0) * 0.85;
	p->hasAtmosphere = rnd(r) < atmoChance;
	p->atmo = atmosphereDefaults();
	if (p->hasAtmosphere) {
		hsv(rnd(r), range(r, 0.35, 0.85), 1.0, &p->atmo.C_R.x);
		p->atmo.shellHeight = (float)(ROCKY_SHELL_FRACTION * b->radius * range(r, 0.5, 1.4) * (dwarf ? 0.5 : 1.0));
		p->atmo.scaleHeight = (float)(0.25 * p->atmo.shellHeight * range(r, 0.7, 1.3));
		p->atmo.K_R = (float)(0.166 * logRange(r, 0.4, 1.8));
		p->atmo.K_M = (float)(0.0025 * logRange(r, 0.5, 2.5));
	} else {
		p->atmo.shellHeight = 0.0f;
	}
	/* Liquid water needs air and an orbit in the habitable zone; recorded
	   for later, drawn only on Earth for now. The roll has its own stream,
	   so the zone can change without reshuffling the rest of the planet. */
	double aAU = 1.0 / (temp * temp);
	Rng wr = { r->s ^ 0x5741544552ull };
	p->hasWater = p->hasAtmosphere && aAU >= HABITABLE_AU_MIN && aAU <= HABITABLE_AU_MAX &&
	              rnd(&wr) < 0.6;

	/* Surface palette: one base hue, varied per biome cell. With no water
	   (humidity 0) only the dry column shows; temperature picks the row. */
	PlanetStyle *s = &p->style;
	*s = baseStyle;
	/* Barren-world hues: mostly greys, ochres, rusts and tans; any other
	   hue only faintly. */
	double hue, sat;
	if (rnd(r) < 0.7) {
		hue = range(r, -0.02, 0.14);
		sat = range(r, 0.1, 0.5);
	} else {
		hue = rnd(r);
		sat = range(r, 0.03, 0.18);
	}
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			hsv(hue + range(r, -0.05, 0.05), sat * range(r, 0.7, 1.2),
			    range(r, 0.25, 0.7) * (1.0 - 0.12 * col), s->biome[row][col]);
	hsv(hue + range(r, -0.1, 0.1), sat * 0.5, range(r, 0.22, 0.5), s->rock);
	hsv(hue, sat * 0.8, range(r, 0.5, 0.8), s->sand);
	/* Dry basins are all salt pans here: keep them close to the ground. */
	float pan = frange(r, 0.95, 1.1);
	for (int c = 0; c < 3; c++)
		s->salt[c] = (s->biome[0][0][c] + s->biome[1][0][c] + s->biome[2][0][c]) / 3.0f * pan;
	hsv(range(r, 0.5, 0.65), range(r, 0.0, 0.15), 0.95, s->snow);
	s->tempOffset = (float)fmin(fmax((temp - 1.0) * 1.2, -1.0), 0.6);
	s->humidity = 0.0f;
	s->equatorLush = 0.0f;
	s->hadleyStrength = 0.0f;
	s->riverThreshold = 2.0f;      /* no rivers */
	s->landRelief = dwarf ? frange(r, 0.02, 0.05) : frange(r, 0.015, 0.035);
	s->oceanRelief = 0.0f;

	/* Terrain recipe: Earth's, varied. Fewer droplets: no rain carving
	   (and a faster bake). */
	TerrainBakeSettings *k = &p->bake;
	*k = baseBake;
	k->seed = (unsigned int)(splitmix(&r->s) & 0x7fffffff);
	k->plateCount = 3 + (int)(rnd(r) * 16.0);
	k->continentArea = frange(r, 0.25, 0.7);
	k->hillAmt *= frange(r, 0.5, 1.6);
	k->plateauH *= frange(r, 0.5, 1.5);
	k->collideH *= frange(r, 0.5, 1.5);
	k->andesH *= frange(r, 0.5, 1.5);
	k->oceanBase *= frange(r, 0.6, 1.3);
	k->hotspotLo = frange(r, 0.4, 0.6);
	k->dropletsPerPx *= frange(r, 0.2, 0.7);
}

/* Cloud layer for a planet with air (none without). Its own RNG stream, so
   adding clouds left every other generated property as it was. Climate
   style for planets that generated water (their moisture map is weather);
   otherwise none through patchy and banded to Venus-like overcast. Heights
   scale with the planet's land relief (the terrain is stylised, ~20x real
   relief), kept inside its air. */
static void generateClouds(unsigned int seed, int id, PlanetInfo *p)
{
	cloudParams *c = &p->clouds;
	*c = cloudDefaults();
	c->style = CLOUD_STYLE_NONE;
	if (!p->hasAtmosphere || p->atmo.shellHeight <= 0.0f)
		return;
	Rng r = rngFor(seed, 3000 + id);
	double u = rnd(&r);
	if (p->hasWater)
		c->style = CLOUD_STYLE_CLIMATE;
	else if (u < 0.2)
		return;
	else if (u < 0.55)
		c->style = CLOUD_STYLE_PATCHY;
	else if (u < 0.8)
		c->style = CLOUD_STYLE_BANDED;
	else
		c->style = CLOUD_STYLE_OVERCAST;

	int overcast = c->style == CLOUD_STYLE_OVERCAST;
	double relief = p->style.landRelief * g_bodies[id].radius;
	double alt = relief * (overcast ? range(&r, CLOUD_GEN_OVERCAST_ALT_MIN, CLOUD_GEN_OVERCAST_ALT_MAX)
	                                : range(&r, CLOUD_GEN_ALTITUDE_MIN, CLOUD_GEN_ALTITUDE_MAX));
	c->altitude = (float)fmin(alt, CLOUD_GEN_MAX_SHELL * p->atmo.shellHeight);
	c->thickness = c->altitude * frange(&r, CLOUD_GEN_THICKNESS_MIN, CLOUD_GEN_THICKNESS_MAX);
	switch (c->style) {
	case CLOUD_STYLE_CLIMATE:
		c->coverage = frange(&r, 0.4, 0.7);
		c->opacity = (float)logRange(&r, 10.0, 40.0);
		break;
	case CLOUD_STYLE_PATCHY:
		c->coverage = frange(&r, 0.15, 0.55);
		c->opacity = (float)logRange(&r, 3.0, 20.0);
		break;
	case CLOUD_STYLE_BANDED:
		c->coverage = frange(&r, 0.3, 0.6);
		c->opacity = (float)logRange(&r, 5.0, 25.0);
		c->bands = frange(&r, 4.0, 14.0);
		break;
	default:
		c->coverage = frange(&r, 0.9, 1.0);
		c->opacity = (float)logRange(&r, 20.0, 60.0);
		break;
	}
	c->weatherScale = frange(&r, 0.3, 0.7);
	c->sharpness = frange(&r, 0.3, 0.8);
	c->detail = frange(&r, 0.25, 0.6);
	/* Flow cycle tied loosely to the spin, kept short enough that the
	   wind belts do not shear the noise into streaks. */
	double spin = fabs(g_bodies[id].rotationPeriod);
	c->flowPeriod = (float)fmin(fmax(spin * range(&r, 0.5, 1.5), 0.25 * 86400.0), 1.5 * 86400.0);
	/* Near-white, faintly tinted (sulphur yellows, dusty tans, cold blues). */
	hsv(rnd(&r), range(&r, 0.0, overcast ? 0.3 : 0.12), range(&r, 0.85, 1.0), c->color);
	c->seed = (unsigned int)(splitmix(&r.s) & 0x7fffffff);
}

static void freeBake(PlanetInfo *p)
{
	if (!p->baked)
		return;
	TerrainGen_FreePlanet(&p->gen);
	Clouds_Drop((int)(p - g_planetInfo));   /* its weather came from that bake */
	glDeleteTextures(1, &p->heightTex);
	p->heightTex = 0;
	p->baked = 0;
}

void SolarSystem_Generate(unsigned int seed)
{
	if (activeBody != BODY_EARTH)
		SolarSystem_Activate(BODY_EARTH, NULL);
	g_systemSeed = seed;
	bakeBody = -1;   /* a bake in flight is for the old system: drop it */

	/* Major orbits: random ratios out of and into Earth's 1 AU. */
	double aAU[MAJOR_COUNT];
	aAU[EARTH_SLOT] = 1.0;
	Rng orbitRng = rngFor(seed, 1000);
	for (int i = EARTH_SLOT - 1; i >= 0; i--)
		aAU[i] = aAU[i + 1] / range(&orbitRng, SYSTEM_SPACING_MIN, SYSTEM_SPACING_MAX);
	for (int i = EARTH_SLOT + 1; i < MAJOR_COUNT; i++)
		aAU[i] = aAU[i - 1] * range(&orbitRng, SYSTEM_SPACING_MIN, SYSTEM_SPACING_MAX);

	for (int i = 0; i < MAJOR_COUNT; i++) {
		int id = majorSlots[i];
		if (id == BODY_EARTH)
			continue;
		Body *b = &g_bodies[id];
		PlanetInfo *p = &g_planetInfo[id];
		freeBake(p);
		memset(p, 0, sizeof *p);
		Rng r = rngFor(seed, id);
		randomOrbit(&r, &b->orbit, aAU[i], 0.08, 4.0);
		int gas = rnd(&r) < gasChance(aAU[i]);
		p->type = gas ? PLANET_GAS : PLANET_ROCKY;
		b->axialTilt = randomTilt(&r);
		if (gas) {
			b->kind = BODY_KIND_SIMPLE;
			b->banded = 1;
			b->radius = logRange(&r, GAS_RADIUS_MIN_M, GAS_RADIUS_MAX_M);
			b->rotationPeriod = range(&r, 0.35, 0.75) * 86400.0;
			/* Gas giant tones, as before: warm tans to cool blues. */
			double hue = rnd(&r) < 0.6 ? range(&r, 0.06, 0.13) : range(&r, 0.5, 0.62);
			hsv(hue, range(&r, 0.2, 0.55), range(&r, 0.75, 0.9), b->color);
			p->hasRings = rnd(&r) < GAS_RING_CHANCE;
		} else {
			b->kind = BODY_KIND_TERRAIN;
			b->banded = 0;
			b->radius = logRange(&r, ROCKY_RADIUS_MIN_M, ROCKY_RADIUS_MAX_M);
			b->rotationPeriod = logRange(&r, 0.4, 60.0) * 86400.0;
			generateRocky(&r, p, b, 1.0 / sqrt(aAU[i]), 0);
			generateClouds(seed, id, p);
			p->hasRings = rnd(&r) < ROCKY_RING_CHANCE;
		}
		p->ringSeed = (unsigned int)(splitmix(&r.s) & 0x7fffffff);
		hsv(range(&r, 0.05, 0.12), range(&r, 0.1, 0.35), range(&r, 0.6, 0.9), p->ringColor);
	}

	/* Dwarfs: Ceres in the gap after the 4th planet, the rest beyond the
	   last one on eccentric, inclined orbits. */
	for (int k = 0; k < 4; k++) {
		int id = k == 0 ? BODY_CERES : kuiperSlots[k - 1];
		Body *b = &g_bodies[id];
		PlanetInfo *p = &g_planetInfo[id];
		freeBake(p);
		memset(p, 0, sizeof *p);
		Rng r = rngFor(seed, id);
		double a = k == 0 ? sqrt(aAU[EARTH_SLOT + 1] * aAU[EARTH_SLOT + 2]) * range(&r, 0.9, 1.1)
		                  : aAU[MAJOR_COUNT - 1] * range(&r, 1.25, 1.7);
		randomOrbit(&r, &b->orbit, a, k == 0 ? 0.1 : 0.3, k == 0 ? 10.0 : 25.0);
		p->type = PLANET_DWARF;
		b->kind = BODY_KIND_TERRAIN;
		b->banded = 0;
		b->radius = logRange(&r, DWARF_RADIUS_MIN_M, DWARF_RADIUS_MAX_M);
		b->rotationPeriod = logRange(&r, 0.2, 6.0) * 86400.0;
		b->axialTilt = randomTilt(&r);
		generateRocky(&r, p, b, 1.0 / sqrt(a), 1);
		generateClouds(seed, id, p);
		p->hasRings = rnd(&r) < DWARF_RING_CHANCE;
		p->ringSeed = (unsigned int)(splitmix(&r.s) & 0x7fffffff);
		hsv(range(&r, 0.05, 0.12), range(&r, 0.1, 0.35), range(&r, 0.6, 0.9), p->ringColor);
	}

	/* Stand-in colours for rocky planets: their dry-biome average. */
	for (int id = 0; id < BODY_COUNT; id++) {
		PlanetInfo *p = &g_planetInfo[id];
		if (p->type != PLANET_ROCKY && p->type != PLANET_DWARF)
			continue;
		for (int c = 0; c < 3; c++)
			g_bodies[id].color[c] = (p->style.biome[0][0][c] + p->style.biome[1][0][c] +
			                         p->style.biome[2][0][c] + p->style.rock[c]) * 0.25f;
	}

	fprintf(stderr, "[system] seed %u:\n", seed);
	for (int id = 0; id < BODY_COUNT; id++) {
		const PlanetInfo *p = &g_planetInfo[id];
		if (p->type == PLANET_NONE)
			continue;
		fprintf(stderr, "  %-9s %-11s a %6.2f AU  r %6.0f km%s%s%s\n", g_bodies[id].name,
		        SolarSystem_TypeName(p->type), g_bodies[id].orbit.a / AU_M, g_bodies[id].radius / 1000.0,
		        p->hasAtmosphere ? "  atmosphere" : "", p->hasRings ? "  rings" : "",
		        p->hasWater ? "  (water)" : "");
		if (p->clouds.style != CLOUD_STYLE_NONE)
			fprintf(stderr, "            clouds: %s, %.0f km up, coverage %.2f\n",
			        Clouds_StyleName(p->clouds.style), p->clouds.altitude / 1000.0, p->clouds.coverage);
	}
}

void SolarSystem_Init(void)
{
	baseStyle = g_planetStyle;
	baseBake = g_terrainBake;

	PlanetInfo *e = &g_planetInfo[BODY_EARTH];
	e->type = PLANET_EARTHLIKE;
	e->hasAtmosphere = 1;
	e->hasWater = 1;
	e->hasRings = 1;
	e->ringSeed = g_terrainBake.seed;   /* Earth's rings follow its terrain seed */
	e->atmo = g_atmosphere;              /* for its stand-in until it is first saved */
	e->clouds = g_clouds;
	e->ringColor[0] = 0.80f; e->ringColor[1] = 0.74f; e->ringColor[2] = 0.66f;
	e->baked = 1;
	activeBody = BODY_EARTH;

	SolarSystem_Generate(g_systemSeed);
}

int SolarSystem_ActiveBody(void)
{
	return activeBody;
}

/* Saves the planet globals into the active planet's record. */
static void saveActive(void)
{
	PlanetInfo *p = &g_planetInfo[activeBody];
	p->gen = g_planetGen;
	p->style = g_planetStyle;
	p->atmo = g_atmosphere;
	p->clouds = g_clouds;
	p->bake = g_terrainBake;
	p->heightTex = planetHeightTex;
}

static void loadActive(int body)
{
	PlanetInfo *p = &g_planetInfo[body];
	g_planetGen = p->gen;
	g_planetStyle = p->style;
	g_atmosphere = p->atmo;
	g_clouds = p->clouds;
	g_terrainBake = p->bake;
	planetHeightTex = p->heightTex;
	activeBody = body;
	/* Earth keeps its fixed stylised ring plane; others ring their equator. */
	const Body *b = &g_bodies[body];
	dvec3 pole = { b->rotation.m[0][2], b->rotation.m[1][2], b->rotation.m[2][2] };
	Rings_SetPlanet(p->hasRings, p->ringSeed, body == BODY_EARTH ? Rings_DefaultNormal() : pole);
}

/* A finished bake of body's terrain (p->heightTex, p->gen set). Dry worlds
   put sea level at the lowest point: all land. */
static void bakeDone(int body)
{
	PlanetInfo *p = &g_planetInfo[body];
	int water = body == BODY_EARTH || (OTHER_PLANET_WATER && p->hasWater);
	if (!water) {
		float gmin, gmax;
		TerrainGen_PlanetRange(&p->gen, &gmin, &gmax);
		p->gen.seaLevel = gmin - 1e-4f;
		p->gen.oceanDepth = 1e-3f;
	}
	p->baked = 1;
}

/* Takes a finished background bake. wait: block until it finishes. */
static void pollBake(int wait)
{
	if (!bakeJob || (!wait && !TerrainGen_BakePoll(bakeJob)))
		return;
	if (bakeBody >= 0) {
		PlanetInfo *p = &g_planetInfo[bakeBody];
		p->heightTex = TerrainGen_BakeFinish(bakeJob, &p->gen);
		bakeDone(bakeBody);
		fprintf(stderr, "[system] %s baked\n", g_bodies[bakeBody].name);
	} else {
		TerrainGen_BakeCancel(bakeJob);
	}
	bakeJob = NULL;
	bakeBody = -1;
}

static int bakeable(int body)
{
	return body >= 0 && g_bodies[body].kind == BODY_KIND_TERRAIN &&
	       g_planetInfo[body].type != PLANET_NONE && g_planetInfo[body].type != PLANET_GAS;
}

/* Starts body's background bake if it needs one and the slot is free. */
static void requestBake(int body)
{
	if (bakeJob || !bakeable(body) || g_planetInfo[body].baked)
		return;
	fprintf(stderr, "[system] baking %s (background)\n", g_bodies[body].name);
	bakeJob = TerrainGen_BakeStart(&g_planetInfo[body].bake, &g_planetInfo[body].style);
	bakeBody = body;
}

/* Bakes p's terrain from p->bake into p now (the bake leaves it in the
   planet globals). One already baking in the background is waited for. */
static void bakeInto(int body, void (*step)(void))
{
	if (bakeJob && bakeBody == body) {
		pollBake(1);
		return;
	}
	PlanetInfo *p = &g_planetInfo[body];
	p->heightTex = generatePlanetHeightmap(&p->bake, &p->style, &p->gen, step);
	bakeDone(body);
}

void SolarSystem_Activate(int body, void (*step)(void))
{
	if (body == activeBody || !bakeable(body))
		return;
	saveActive();
	if (!g_planetInfo[body].baked) {
		fprintf(stderr, "[system] baking %s\n", g_bodies[body].name);
		bakeInto(body, step);
	}
	loadActive(body);
}

void SolarSystem_Update(dvec3 cam, int focusBody, int blocking)
{
	pollBake(0);
	int want = SolarSystem_PickActive(cam);
	if (want != activeBody) {
		if (g_planetInfo[want].baked || blocking)
			SolarSystem_Activate(want, NULL);
		else
			requestBake(want);   /* stays a stand-in until it is done */
	}
	/* Nothing wanted now: get the focused planet ready ahead of time. */
	if (!blocking)
		requestBake(focusBody);
}

int SolarSystem_BakingBody(float *progress)
{
	if (!bakeJob || bakeBody < 0)
		return -1;
	if (progress)
		*progress = TerrainGen_BakeProgress(bakeJob);
	return bakeBody;
}

void SolarSystem_RebakeActive(void (*step)(void))
{
	PlanetInfo *p = &g_planetInfo[activeBody];
	saveActive();
	freeBake(p);
	bakeInto(activeBody, step);
	if (activeBody == BODY_EARTH)
		p->ringSeed = p->bake.seed;
	loadActive(activeBody);
}

int SolarSystem_PickActive(dvec3 cam)
{
	int best = activeBody;
	double bestPx = Planets_ScreenRadiusPx(&g_bodies[activeBody], cam);
	for (int id = 0; id < BODY_COUNT; id++) {
		if (g_bodies[id].kind != BODY_KIND_TERRAIN)
			continue;
		const Body *b = &g_bodies[id];
		double d = dlengthvec3(dsubvec3(b->position, cam));
		double px = d <= b->radius ? 1e9 : Planets_ScreenRadiusPx(b, cam);
		if (px >= ACTIVATE_PX && px > bestPx * 1.25) {
			best = id;
			bestPx = px;
		}
	}
	return best;
}

const char *SolarSystem_TypeName(PlanetType t)
{
	switch (t) {
	case PLANET_EARTHLIKE: return "Earth-like";
	case PLANET_ROCKY:     return "Rocky";
	case PLANET_DWARF:     return "Dwarf";
	case PLANET_GAS:       return "Gas giant";
	default:               return "-";
	}
}
