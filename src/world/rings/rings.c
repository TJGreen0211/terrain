#include "engine/gfx/shader.h"
#include "engine/platform/window.h"
#include "world/rings/rings.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <stdlib.h>

/* Ring geometry in planet radii. Saturn's main rings span ~1.24-2.27. */
/* Ring-plane tilt (degrees): normal = rotX(TILT_X) * rotZ(TILT_Z) * +Y.
   The sun sits in the planet's XZ plane, so an untilted ring would be
   edge-on to it (unlit, no shadow). These put the sun ~18 deg above the
   plane, on the same side as the spawn view. */
#define RING_TILT_Z 35.0
#define RING_TILT_X 10.0
/* Sun angular radius (radians): sets the penumbra width of the planet's
   shadow on the ring and of the ring's shadow on everything else. */
#define RING_SUN_ANGLE ((float)SUN_ANGULAR_RADIUS_RAD)
#define RING_PROFILE_SIZE 16384
/* Baked profiles kept at once (each ~0.5 MB with mips), one per ring seed:
   the full passes' planet and every ringed stand-in in view. Least
   recently used goes first. */
#define RING_PROFILE_CACHE 16
/* Texture unit for ringProfile in every program; TerrainDraw_SetBodyUniforms uses 0-8. */
#define RING_PROFILE_UNIT 9

/* Close-up field. The sheet fades out over RING_FADE_* (distance from the
   camera, as fractions of ROCK_RANGE) where the rocks and dust take over.
   Sizes in metres come from WorldConfig.h. */
#define ROCK_RANGE ((float)RING_ROCK_RANGE_M)
#define ROCK_CELL ((float)RING_ROCK_CELL_M)
#define ROCK_DENSITY 0.35f          /* spawn probability per unit optical depth, per mesh */
#define ROCK_MIN_SIZE ((float)RING_ROCK_MIN_SIZE_M)
#define ROCK_MAX_SIZE ((float)RING_ROCK_MAX_SIZE_M)
#define ROCK_MESH_SCALE (1.0f/1.6f) /* rock*.obj extents (~1.6) -> unit radius */
#define RING_HALF_THICKNESS ((float)RING_HALF_THICKNESS_M)
#define RING_FADE_NEAR 0.35f
#define RING_FADE_FAR 0.9f
#define DUST_RANGE ((float)RING_DUST_RANGE_M)
#define DUST_CELL ((float)RING_DUST_CELL_M)
#define DUST_PER_CELL 3
#define DUST_DENSITY 0.6f
#define DUST_HALF_THICKNESS ((float)RING_DUST_HALF_THICKNESS_M)

static struct {
	unsigned int seed;
	GLuint tex;
	unsigned int lastUse;   /* 0 = empty slot */
} g_profiles[RING_PROFILE_CACHE];
static unsigned int g_profileClock;

static struct {
	GLuint profileTex;      /* the full passes' planet's (g_profiles) */
	GLuint sheetShader, rockShader, dustShader;
	GLuint emptyVAO;
	GLuint rockVAO[3];
	int rockVerts[3];
	unsigned int seed;      /* rock / dust spawn hash */
	int enabled;            /* the full passes' planet has rings (Rings_SetPlanet) */
	int receivers;          /* ...and it is the planet the receivers draw (Rings_SetFrame) */

	/* Ring frame (world, constant): normal + in-plane basis. */
	dvec3 n, u, v;

	/* Per frame (Rings_SetFrame). */
	vec3 center;          /* camera-relative planet centre */
	vec3 light;           /* camera-relative light */
	double camLocalX, camLocalY, camHeight;
	float inner, outer, planetRadius;
} g_ring;

/* ---- Profile bake ------------------------------------------------------ */

static unsigned int pcgHash(unsigned int v)
{
	unsigned int s = v * 747796405u + 2891336453u;
	unsigned int w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}

static float rnd(unsigned int *state)
{
	*state = pcgHash(*state);
	return (float)(*state / 4294967296.0);
}

static float rndRange(unsigned int *state, float a, float b)
{
	return a + (b - a) * rnd(state);
}

static float hashf(unsigned int seed, int i)
{
	return (float)(pcgHash(seed ^ pcgHash((unsigned int)i)) / 4294967296.0);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float smoothstepf(float e0, float e1, float x)
{
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

static float noise1(unsigned int seed, float x)
{
	float fl = floorf(x);
	int i = (int)fl;
	float f = x - fl;
	f = f * f * (3.0f - 2.0f * f);
	return lerpf(hashf(seed, i), hashf(seed, i + 1), f);
}

/* Value noise that holds each lattice value and jumps between them over the
   middle 40% of the cell: plateaus with abrupt steps, the character of real
   ringlet structure, where smooth noise only gives sine-like wiggles. */
static float stepNoise(unsigned int seed, float x)
{
	float fl = floorf(x);
	int i = (int)fl;
	return lerpf(hashf(seed, i), hashf(seed, i + 1), smoothstepf(0.3f, 0.7f, x - fl));
}

/* 1D fbm in [0,1] (colour only). */
static float fbm1(unsigned int seed, float x, int octaves)
{
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; o++) {
		sum += amp * noise1(seed + (unsigned int)o * 1013u, x);
		norm += amp;
		amp *= 0.5f;
		x *= 2.13f;
	}
	return sum / norm;
}

#define RING_TEXEL (1.0f / RING_PROFILE_SIZE)
/* Sharp edges: ~1.5 texels, about 25 m on Saturn's scale. */
#define RING_EDGE (1.5f * RING_TEXEL)

/* 1 inside [a,b] with soft edges. */
static float band(float r, float a, float b, float soft)
{
	return smoothstepf(a - soft, a + soft, r) * (1.0f - smoothstepf(b - soft, b + soft, r));
}

/* Multiplicative cascade: each octave scales the density by
   exp(sigma_o * (2n - 1)), so structure is intermittent (calm stretches,
   sudden jumps) at every scale from the ring width down to a few texels.
   Odd octaves use stepNoise for sharp-edged ringlets. Octave weights decay
   slowly (0.85) so the fine scales stay visible. */
#define CASCADE_OCTAVES 9
static float cascade(unsigned int seed, float r, float sigma)
{
	float s = 0.0f, w = 1.0f, freq = 6.0f;
	for (int o = 0; o < CASCADE_OCTAVES; o++) {
		unsigned int os = seed + (unsigned int)o * 7919u;
		float n = (o & 1) ? stepNoise(os, r * freq) : noise1(os, r * freq);
		s += sigma * w * (2.0f * n - 1.0f);
		w *= 0.85f;
		freq *= 2.1f;   /* finest octave ~2300 cycles: >= 7 texels each */
	}
	return expf(s);
}

/* Ring radius r in [0,1] <-> distance from the planet centre in planet
   radii (orbital resonances are ratios of real radii). */
static float rhoOf(float r) { return RING_INNER + r * (RING_OUTER - RING_INNER); }
static float rOf(float rho) { return (rho - RING_INNER) / (RING_OUTER - RING_INNER); }

typedef struct { float pos, width, floor, ringletTau; } RingGap;
typedef struct { float pos, amp, damp, k, phase; } RingWave;
typedef struct { float lo, hi, tau; } RingBand;
typedef struct { float rho, mass; } RingMoon;

#define MAX_GAPS 32
#define MAX_WAVES 128
#define MAX_BANDS 16
#define MAX_MOONS 5

static int addGap(RingGap *g, int n, float pos, float width, float floor, float ringletTau)
{
	if (n >= MAX_GAPS || width < 2.0f * RING_TEXEL)
		return n;
	g[n].pos = pos; g[n].width = width; g[n].floor = floor; g[n].ringletTau = ringletTau;
	return n + 1;
}

static int addBand(RingBand *b, int n, float lo, float hi, float tau)
{
	if (n >= MAX_BANDS)
		return n;
	b[n].lo = lo; b[n].hi = hi; b[n].tau = tau;
	return n + 1;
}

/* Saturn-like profile with seeded proportions, built from the physics that
   shapes real rings:

   - Zones: faint C ring, dense B ring, Cassini division, A ring, F ring,
     with sharp (1-2 texel) outer edges.
   - Moons outside the ring. Gaps and waves sit at their inner Lindblad
     resonances, rho = rho_moon * (q/p)^(2/3) for a p:q period ratio, so a
     seed gives a different but physically consistent layout. A "Mimas"
     places the B ring's outer edge at its 2:1 (opening the Cassini
     division), a "Janus" holds the A ring's outer edge at its 7:6, and
     small shepherds near the edge add a cluster of waves to the outer A
     ring, as Prometheus/Pandora do.
   - Density waves at the weaker resonances: damped chirps whose wavelength
     shrinks with distance x outward from the resonance (phase k*x^2), with
     a strongly non-linear (peaky) profile.
   - Sharp gaps in the C ring and Cassini division (some holding narrow
     dense ringlets), and Encke/Keeler-like gaps cleared by embedded
     moonlets in the A ring.
   - A multiplicative cascade for the uneven multi-scale ringlet structure
     (strongest in the B ring) and sharp-edged plateaus in the C ring.

   Colour runs from dark dust (thin) to bright ice (dense), with a seeded
   tint. */
static void bakeProfile(unsigned int seed, GLuint tex)
{
	unsigned int st = seed * 2654435761u + 0x51ED27u;

	/* Zones. bEnd is the Mimas 2:1, aEnd the Janus 7:6. */
	float bStart = rndRange(&st, 0.20f, 0.30f);
	float bEnd = rndRange(&st, 0.58f, 0.66f);
	float aStart = bEnd + rndRange(&st, 0.035f, 0.07f);
	float aEnd = rndRange(&st, 0.92f, 0.96f);
	float fPos = fminf(aEnd + rndRange(&st, 0.015f, 0.035f), 0.99f);
	float tauC = rndRange(&st, 0.06f, 0.14f);
	float tauB = rndRange(&st, 1.3f, 2.5f);
	float tauCass = rndRange(&st, 0.04f, 0.10f);
	float tauA = rndRange(&st, 0.45f, 0.8f);

	RingMoon moons[MAX_MOONS];
	int moonCount = 0;
	moons[moonCount].rho = rhoOf(bEnd) * powf(2.0f, 2.0f / 3.0f);
	moons[moonCount++].mass = 3.0f;
	moons[moonCount].rho = rhoOf(aEnd) * powf(7.0f / 6.0f, 2.0f / 3.0f);
	moons[moonCount++].mass = 1.5f;
	int shepherds = 2 + (rnd(&st) < 0.5f);
	for (int i = 0; i < shepherds && moonCount < MAX_MOONS; i++) {
		moons[moonCount].rho = RING_OUTER * rndRange(&st, 1.01f, 1.12f);
		moons[moonCount++].mass = rndRange(&st, 0.3f, 0.8f);
	}

	RingGap gaps[MAX_GAPS];
	RingWave waves[MAX_WAVES];
	RingBand bands[MAX_BANDS];
	int gapCount = 0, waveCount = 0, bandCount = 0;

	/* Resonances: first order (p:p-1) is strong, second order (p:p-2) weak.
	   The two edge-defining ones are already the zone edges. */
	for (int m = 0; m < moonCount; m++) {
		for (int order = 1; order <= 2; order++) {
			for (int p = order + 1; p <= 40; p++) {
				if ((m == 0 && p == 2 && order == 1) || (m == 1 && p == 7 && order == 1))
					continue;
				float r = rOf(moons[m].rho * powf((float)(p - order) / p, 2.0f / 3.0f));
				if (r <= 0.01f || r >= aEnd - 0.002f)
					continue;
				float s = moons[m].mass * (order == 1 ? 1.0f : 0.12f) * sqrtf((float)p) * 0.3f;
				if (s < 0.15f)
					continue;
				if (s > 2.5f) {
					gapCount = addGap(gaps, gapCount, r, 0.0015f * s, 0.03f, 0.0f);
					continue;
				}
				if (waveCount >= MAX_WAVES)
					continue;
				RingWave *w = &waves[waveCount++];
				w->pos = r;
				w->amp = fminf(0.15f + 0.35f * s, 0.9f);
				w->damp = (0.0025f + 0.004f * fminf(s, 1.5f)) * rndRange(&st, 0.7f, 1.3f);
				/* N crests before damping, capped so the shortest wavelength
				   (damp / 2N at x = damp) stays >= 6 texels. */
				float crests = fminf(rndRange(&st, 8.0f, 20.0f), w->damp / (6.0f * RING_TEXEL) * 0.5f);
				w->k = 2.0f * (float)M_PI * crests / (w->damp * w->damp);
				w->phase = rndRange(&st, -0.5f, 0.5f);
			}
		}
	}

	/* C ring: sharp-edged plateaus and gaps (Colombo/Maxwell-like), some
	   gaps holding a narrow dense ringlet. */
	int plateaus = 3 + (int)(rnd(&st) * 4.0f);
	for (int i = 0; i < plateaus; i++) {
		float lo = rndRange(&st, 0.03f, bStart - 0.03f);
		bandCount = addBand(bands, bandCount, lo, lo + rndRange(&st, 0.004f, 0.016f), tauC * rndRange(&st, 2.0f, 4.0f));
	}
	int cGaps = 3 + (int)(rnd(&st) * 4.0f);
	for (int i = 0; i < cGaps; i++)
		gapCount = addGap(gaps, gapCount, rndRange(&st, 0.03f, bStart - 0.01f), rndRange(&st, 0.001f, 0.004f),
		                  0.0f, rnd(&st) < 0.5f ? rndRange(&st, 0.3f, 0.9f) : 0.0f);

	/* Cassini division: a Huygens-like gap with its ringlet at the inner
	   edge, dense ringlets, and a few narrow gaps. */
	gapCount = addGap(gaps, gapCount, bEnd + 0.003f, rndRange(&st, 0.004f, 0.006f), 0.0f, rndRange(&st, 0.5f, 1.0f));
	int cassRinglets = 2 + (int)(rnd(&st) * 3.0f);
	for (int i = 0; i < cassRinglets; i++) {
		float lo = rndRange(&st, bEnd + 0.01f, aStart - 0.008f);
		bandCount = addBand(bands, bandCount, lo, lo + rndRange(&st, 0.002f, 0.006f), rndRange(&st, 0.2f, 0.6f));
	}
	int cassGaps = 2 + (int)(rnd(&st) * 3.0f);
	for (int i = 0; i < cassGaps; i++)
		gapCount = addGap(gaps, gapCount, rndRange(&st, bEnd + 0.01f, aStart - 0.004f), rndRange(&st, 0.001f, 0.003f),
		                  0.0f, rnd(&st) < 0.5f ? rndRange(&st, 0.2f, 0.5f) : 0.0f);

	/* A ring: Encke- and Keeler-like gaps cleared by embedded moonlets. */
	gapCount = addGap(gaps, gapCount, aStart + (aEnd - aStart) * rndRange(&st, 0.75f, 0.88f),
	                  rndRange(&st, 0.004f, 0.007f), 0.0f, rndRange(&st, 0.05f, 0.15f));
	gapCount = addGap(gaps, gapCount, aEnd - rndRange(&st, 0.004f, 0.01f), rndRange(&st, 0.0008f, 0.0015f), 0.0f, 0.0f);

	/* Dense-ice colour: dusty tan through to blue-grey. */
	float tint = rnd(&st);
	float dust[3] = { 0.28f, 0.25f, 0.22f };
	float ice[3] = { lerpf(0.66f, 0.60f, tint), lerpf(0.58f, 0.63f, tint), lerpf(0.46f, 0.66f, tint) };
	unsigned int ns = pcgHash(seed ^ 0xA5A5A5A5u);

	float *texels = malloc(RING_PROFILE_SIZE * 4 * sizeof(float));
	for (int i = 0; i < RING_PROFILE_SIZE; i++) {
		float r = (i + 0.5f) * RING_TEXEL;

		float wC = band(r, 0.0f, bStart, RING_EDGE);
		float wB = band(r, bStart, bEnd, RING_EDGE);
		float wCass = band(r, bEnd, aStart, RING_EDGE);
		float wA = band(r, aStart, aEnd, RING_EDGE);

		/* Base zones: C rises gently, B ramps up over its inner edge to a
		   denser core, A thins outward. */
		float tau = tauC * (0.6f + 0.4f * r / bStart) * smoothstepf(0.0f, 0.04f, r) * wC
		          + tauB * smoothstepf(bStart, bStart + 0.025f, r) * (0.8f + 0.4f * smoothstepf(bStart, bStart + 0.15f, r)) * wB
		          + tauCass * wCass
		          + tauA * (1.0f - 0.3f * (r - aStart) / (aEnd - aStart)) * wA;

		/* Multi-scale structure, strongest in the B ring. */
		tau *= cascade(ns + 1u, r, 0.3f * wC + 0.55f * wB + 0.3f * wCass + 0.25f * wA);

		for (int b = 0; b < bandCount; b++)
			tau += bands[b].tau * band(r, bands[b].lo, bands[b].hi, RING_EDGE);

		/* Density waves propagate outward from their resonance. Non-linear
		   waves have sharp crests and broad troughs: exp(A cos) gives that. */
		for (int w = 0; w < waveCount; w++) {
			float x = r - waves[w].pos;
			if (x <= 0.0f || x > 2.5f * waves[w].damp)
				continue;
			float xd = x / waves[w].damp;
			float env = smoothstepf(0.0f, 2.0f * RING_TEXEL, x) * expf(-xd * xd * xd);
			tau *= expf(waves[w].amp * env * cosf(waves[w].k * x * x + waves[w].phase));
		}

		/* Gaps last, so they cut cleanly through everything above; a
		   ringlet inside a gap is added back after the cut. */
		for (int g = 0; g < gapCount; g++) {
			float half = 0.5f * gaps[g].width;
			float d = fabsf(r - gaps[g].pos);
			if (d > half + RING_EDGE)
				continue;
			tau *= lerpf(gaps[g].floor, 1.0f, smoothstepf(half - RING_EDGE, half + RING_EDGE, d));
			if (gaps[g].ringletTau > 0.0f) {
				float rw = fmaxf(0.12f * gaps[g].width, 2.0f * RING_TEXEL);
				tau += gaps[g].ringletTau * band(r, gaps[g].pos - 0.5f * rw, gaps[g].pos + 0.5f * rw, RING_EDGE);
			}
		}

		/* F ring: two braided strands. */
		float df1 = (r - fPos) / 0.0008f, df2 = (r - fPos - 0.0012f) / 0.0005f;
		tau += 0.5f * expf(-df1 * df1) + 0.2f * expf(-df2 * df2);
		tau = fmaxf(tau, 0.0f);

		float dense = smoothstepf(0.03f, 1.6f, tau);
		float bright = 0.85f + 0.3f * fbm1(ns + 6u, r * 14.0f, 3);
		float hue = (fbm1(ns + 7u, r * 35.0f, 2) - 0.5f) * 0.3f;
		float col[3];
		for (int c = 0; c < 3; c++)
			col[c] = lerpf(dust[c], ice[c], dense) * bright;
		col[0] *= 1.0f + hue;
		col[2] *= 1.0f - hue;
		/* A ring a little greyer than B. */
		float aW = 0.3f * band(r, aStart, aEnd, 0.01f);
		float lum = 0.3f * col[0] + 0.59f * col[1] + 0.11f * col[2];
		for (int c = 0; c < 3; c++) {
			col[c] = lerpf(col[c], lum, aW);
			texels[i * 4 + c] = fminf(fmaxf(col[c], 0.02f), 0.95f);
		}
		texels[i * 4 + 3] = tau;
	}
	/* Clamp-to-edge reads the end texels outside [0,1]: keep them empty. */
	texels[3] = 0.0f;
	texels[(RING_PROFILE_SIZE - 1) * 4 + 3] = 0.0f;

	glBindTexture(GL_TEXTURE_1D, tex);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F, RING_PROFILE_SIZE, 0, GL_RGBA, GL_FLOAT, texels);
	glGenerateMipmap(GL_TEXTURE_1D);
	glBindTexture(GL_TEXTURE_1D, 0);
	free(texels);
}

/* ---- Setup ------------------------------------------------------------- */

static dvec3 dnormalize(dvec3 a)
{
	return dscalarvec3(a, 1.0 / dlengthvec3(a));
}

static dvec3 dcross(dvec3 a, dvec3 b)
{
	dvec3 c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	return c;
}

static double ddot(dvec3 a, dvec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* The baked profile for seed, baking it into the least recently used slot
   on a miss (a few ms). */
static GLuint profileFor(unsigned int seed)
{
	int slot = 0;
	for (int i = 0; i < RING_PROFILE_CACHE; i++) {
		if (g_profiles[i].lastUse && g_profiles[i].seed == seed) {
			g_profiles[i].lastUse = ++g_profileClock;
			return g_profiles[i].tex;
		}
		if (g_profiles[i].lastUse < g_profiles[slot].lastUse)
			slot = i;
	}
	if (!g_profiles[slot].tex) {
		glGenTextures(1, &g_profiles[slot].tex);
		glBindTexture(GL_TEXTURE_1D, g_profiles[slot].tex);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_1D, 0);
	}
	bakeProfile(seed, g_profiles[slot].tex);
	g_profiles[slot].seed = seed;
	g_profiles[slot].lastUse = ++g_profileClock;
	return g_profiles[slot].tex;
}

void Rings_Init(unsigned int seed, const GLuint rockVAO[3], const int rockVerts[3])
{
	Shader_Create(&g_ring.sheetShader, "world/rings/ring.vert", "world/rings/ring.frag");
	Shader_Create(&g_ring.rockShader, "world/rings/rock.vert", "world/rings/rock.frag");
	Shader_Create(&g_ring.dustShader, "world/rings/ringdust.vert", "world/rings/ringdust.frag");
	/* The sheet and dust are generated from gl_VertexID. */
	glGenVertexArrays(1, &g_ring.emptyVAO);
	for (int i = 0; i < 3; i++) {
		g_ring.rockVAO[i] = rockVAO[i];
		g_ring.rockVerts[i] = rockVerts[i];
	}

	Rings_SetPlanet(1, seed, Rings_DefaultNormal());

	GLuint own[3] = { g_ring.sheetShader, g_ring.rockShader, g_ring.dustShader };
	for (int i = 0; i < 3; i++) {
		glUseProgram(own[i]);
		glUniform1i(glGetUniformLocation(own[i], "ringProfile"), RING_PROFILE_UNIT);
	}
}

dvec3 Rings_DefaultNormal(void)
{
	double tz = RING_TILT_Z * M_PI / 180.0, tx = RING_TILT_X * M_PI / 180.0;
	dvec3 n = { -sin(tz), cos(tz) * cos(tx), cos(tz) * sin(tx) };
	return dnormalize(n);
}

void Rings_SetPlanet(int enabled, unsigned int seed, dvec3 normal)
{
	g_ring.enabled = enabled;
	g_ring.n = dnormalize(normal);
	dvec3 ref = { 0.0, 0.0, 1.0 };
	if (fabs(g_ring.n.z) > 0.99) { ref.x = 1.0; ref.z = 0.0; }
	g_ring.u = dnormalize(dcross(g_ring.n, ref));
	g_ring.v = dcross(g_ring.n, g_ring.u);
	if (enabled) {
		g_ring.profileTex = profileFor(seed);
		g_ring.seed = pcgHash(seed);
	}
}

void Rings_AttachReceiver(GLuint shader)
{
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "ringProfile"), RING_PROFILE_UNIT);
	glUniform1f(glGetUniformLocation(shader, "hasRing"), 0.0f);
}

void Rings_SetFrame(dvec3 planetWorld, dvec3 camWorld, float planetRadius, vec4 lightPosition, int receivers)
{
	g_ring.receivers = receivers;
	dvec3 rel = dsubvec3(camWorld, planetWorld);
	g_ring.camLocalX = ddot(rel, g_ring.u);
	g_ring.camLocalY = ddot(rel, g_ring.v);
	g_ring.camHeight = ddot(rel, g_ring.n);
	g_ring.center = dvec3tovec3(dscalarvec3(rel, -1.0));
	g_ring.light.x = lightPosition.x;
	g_ring.light.y = lightPosition.y;
	g_ring.light.z = lightPosition.z;
	g_ring.planetRadius = planetRadius;
	g_ring.inner = planetRadius * RING_INNER;
	g_ring.outer = planetRadius * RING_OUTER;
}

static void bindProfile(void)
{
	glActiveTexture(GL_TEXTURE0 + RING_PROFILE_UNIT);
	glBindTexture(GL_TEXTURE_1D, g_ring.profileTex);
	glActiveTexture(GL_TEXTURE0);
}

static void setVec3(GLuint shader, const char *name, vec3 a)
{
	glUniform3f(glGetUniformLocation(shader, name), a.x, a.y, a.z);
}

static void setDVec3(GLuint shader, const char *name, dvec3 a)
{
	glUniform3f(glGetUniformLocation(shader, name), (float)a.x, (float)a.y, (float)a.z);
}

/* Uniforms shared by the receivers and the ring's own passes. */
static void setRingShape(GLuint shader)
{
	setVec3(shader, "ringCenter", g_ring.center);
	setDVec3(shader, "ringNormal", g_ring.n);
	glUniform2f(glGetUniformLocation(shader, "ringRadii"), g_ring.inner, g_ring.outer);
	glUniform1f(glGetUniformLocation(shader, "ringSunAngle"), RING_SUN_ANGLE);
	glUniform2f(glGetUniformLocation(shader, "ringFade"), ROCK_RANGE * RING_FADE_NEAR, ROCK_RANGE * RING_FADE_FAR);
}

void Rings_BindReceiver(GLuint shader)
{
	glUseProgram(shader);
	glUniform1f(glGetUniformLocation(shader, "hasRing"), g_ring.enabled && g_ring.receivers ? 1.0f : 0.0f);
	setRingShape(shader);
	bindProfile();
}

void Rings_BindStandIn(GLuint shader, unsigned int seed)
{
	glUniform1i(glGetUniformLocation(shader, "ringProfile"), RING_PROFILE_UNIT);
	glUniform1f(glGetUniformLocation(shader, "ringSunAngle"), RING_SUN_ANGLE);
	glActiveTexture(GL_TEXTURE0 + RING_PROFILE_UNIT);
	glBindTexture(GL_TEXTURE_1D, profileFor(seed));
	glActiveTexture(GL_TEXTURE0);
}

/* Program setup for the ring's own passes (sheet, rocks, dust). */
static void useRingProgram(GLuint shader, mat4 proj, mat4 view)
{
	glUseProgram(shader);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &view.m[0][0]);
	setRingShape(shader);
	setDVec3(shader, "ringU", g_ring.u);
	setDVec3(shader, "ringV", g_ring.v);
	setVec3(shader, "lightPosition", g_ring.light);
	glUniform1f(glGetUniformLocation(shader, "planetRadius"), g_ring.planetRadius);
	glUniform1f(glGetUniformLocation(shader, "time"), (float)glfwGetTime());
	bindProfile();
}

/* Hashed spawn grid: a gridN x gridN window of ring-plane cells around the
   camera. The base cell is floored in double so the window never jitters,
   and the camera's plane offset is passed separately so the shaders only
   ever subtract nearby coordinates. Returns 0 when the camera is too far
   from the ring for the field to be visible. */
static int setSpawnGrid(GLuint shader, float cell, float range, float halfThickness, int *gridN)
{
	double rc = sqrt(g_ring.camLocalX * g_ring.camLocalX + g_ring.camLocalY * g_ring.camLocalY);
	if (fabs(g_ring.camHeight) > range + halfThickness || rc < g_ring.inner - range || rc > g_ring.outer + range)
		return 0;
	int half = (int)ceilf(range / cell);
	*gridN = 2 * half + 1;
	glUniform2i(glGetUniformLocation(shader, "baseCell"),
	            (int)floor(g_ring.camLocalX / cell), (int)floor(g_ring.camLocalY / cell));
	glUniform2f(glGetUniformLocation(shader, "camLocal"), (float)g_ring.camLocalX, (float)g_ring.camLocalY);
	glUniform1f(glGetUniformLocation(shader, "camHeight"), (float)g_ring.camHeight);
	glUniform1i(glGetUniformLocation(shader, "gridN"), *gridN);
	glUniform1f(glGetUniformLocation(shader, "cellSize"), cell);
	glUniform1f(glGetUniformLocation(shader, "range"), range);
	glUniform1f(glGetUniformLocation(shader, "halfThickness"), halfThickness);
	return 1;
}

/* ---- Draws ------------------------------------------------------------- */

void Rings_DrawRocks(mat4 proj, mat4 view)
{
	GLuint s = g_ring.rockShader;
	int gridN;
	if (!g_ring.enabled)
		return;
	useRingProgram(s, proj, view);
	if (!setSpawnGrid(s, ROCK_CELL, ROCK_RANGE, RING_HALF_THICKNESS, &gridN))
		return;
	glUniform1f(glGetUniformLocation(s, "density"), ROCK_DENSITY);
	glUniform2f(glGetUniformLocation(s, "sizeRange"), ROCK_MIN_SIZE, ROCK_MAX_SIZE);
	glUniform1f(glGetUniformLocation(s, "meshScale"), ROCK_MESH_SCALE);
	/* rock.frag faces the normal toward the viewer, so the meshes' winding
	   does not matter. */
	glDisable(GL_CULL_FACE);
	for (int i = 0; i < 3; i++) {
		glUniform1ui(glGetUniformLocation(s, "seed"), g_ring.seed ^ pcgHash(0x9E37u + (unsigned int)i));
		glBindVertexArray(g_ring.rockVAO[i]);
		glDrawArraysInstanced(GL_TRIANGLES, 0, g_ring.rockVerts[i], gridN * gridN);
	}
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
}

/* Premultiplied-alpha passes that test against the scene depth but do not
   write it. */
static void beginTranslucent(void)
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
}

static void endTranslucent(void)
{
	glEnable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void Rings_DrawSheet(mat4 proj, mat4 view)
{
	if (!g_ring.enabled)
		return;
	useRingProgram(g_ring.sheetShader, proj, view);
	beginTranslucent();
	glBindVertexArray(g_ring.emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
	endTranslucent();
}

void Rings_DrawDust(mat4 proj, mat4 view)
{
	GLuint s = g_ring.dustShader;
	int gridN;
	if (!g_ring.enabled)
		return;
	useRingProgram(s, proj, view);
	if (!setSpawnGrid(s, DUST_CELL, DUST_RANGE, DUST_HALF_THICKNESS, &gridN))
		return;
	glUniform1ui(glGetUniformLocation(s, "seed"), g_ring.seed ^ 0xD057u);
	glUniform1i(glGetUniformLocation(s, "perCell"), DUST_PER_CELL);
	glUniform1f(glGetUniformLocation(s, "density"), DUST_DENSITY);
	/* Pixels per world unit at unit distance. */
	glUniform1f(glGetUniformLocation(s, "projScale"), Window_Height() / (2.0f * tanf(0.5f * (float)CAMERA_FOV_Y_DEG * (float)M_PI / 180.0f)));
	beginTranslucent();
	glEnable(GL_PROGRAM_POINT_SIZE);
	glBindVertexArray(g_ring.emptyVAO);
	glDrawArrays(GL_POINTS, 0, gridN * gridN * DUST_PER_CELL);
	glBindVertexArray(0);
	glDisable(GL_PROGRAM_POINT_SIZE);
	endTranslucent();
}
