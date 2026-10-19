#include "Main.h"
#include "TerrainGen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h>

/* The project builds without /O2; the bake's CPU erosion needs real
   optimization to stay inside the 1-3 s startup budget. Scoped to this
   translation unit so no other module's codegen changes. */
#pragma optimize("gtp", on)

/* Sea level + ocean depth used by quad.frag/water.frag (see TerrainGen.h). */
PlanetGenParams g_planetGen = { 0.5f, 0.22f };

#define HEIGHTMAP_W 2048
#define HEIGHTMAP_H 1024

/* ---- Bake pipeline (tweak here; shaping recipe constants live in
   shaders/heightbake.frag) ---- */
#define THERMAL_PASSES       12      /* weathering iterations */
#define THERMAL_TALUS        0.010f  /* max stable height difference per texel */

#define DROPLETS_PER_PX      0.15f   /* droplet count = per-px * W * H (tune) */
#define DROPLET_MAX_STEPS    512
#define DROPLET_STEP         3.0f    /* advection length, in texels */
#define DROPLET_GRAVITY      5.0f
#define DROPLET_INERTIA      0.05f
#define DROPLET_SEDIMENT_MAX 20.0f   /* sediment capacity coefficient */
#define DROPLET_DEPOSIT_RATE 0.35f   /* fraction of surplus deposited per step */
#define DROPLET_EROSION_RATE 1.0f
#define DROPLET_START_WATER  1.0f
#define DROPLET_MIN_WATER    0.05f
#define DROPLET_EVAP_RATE    0.06f   /* fraction of water evaporated per step */

#define NORMALIZE_LO_PCT     0.01f   /* percentile stretch of the relief */
#define NORMALIZE_HI_PCT     0.99f
#define WATER_COVERAGE       0.55f   /* sea level = this height percentile */
#define OCEAN_DEPTH_PCT      0.95f   /* depth normalizing percentile */

/* ---------------- CPU height field helpers ---------------- */

/* Replace NaN/Inf so no poison reaches qsort/histograms/the mip chain. */
static void clampFinite(float *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float v = buf[i];
		if (!(v == v)) v = 0.0f;
		else if (v > FLT_MAX) v = FLT_MAX;
		else if (v < -FLT_MAX) v = -FLT_MAX;
		buf[i] = v;
	}
}

/* Coarse percentile via histogram (no sort): with 2048+ bins over a smooth
   distribution the error is well below one texel, and it keeps the 2M-value
   percentiles off the startup critical path (MSVC qsort measured ~1.7 s). */
#define HIST_BINS 2048
static float histPercentile(const float *buf, size_t n, float p)
{
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (size_t i = 0; i < n; i++) {
		if (buf[i] < lo) lo = buf[i];
		if (buf[i] > hi) hi = buf[i];
	}
	if (!(hi > lo)) return lo;
	static unsigned int hist[HIST_BINS];
	memset(hist, 0, sizeof hist);
	double scale = (double)(HIST_BINS - 1) / (double)(hi - lo);
	for (size_t i = 0; i < n; i++) {
		int b = (int)((double)(buf[i] - lo) * scale);
		hist[b < 0 ? 0 : (b > HIST_BINS - 1 ? HIST_BINS - 1 : b)]++;
	}
	size_t target = (size_t)(p * (double)n);
	size_t acc = 0;
	int bin = HIST_BINS - 1;
	for (int b = 0; b < HIST_BINS; b++) {
		acc += hist[b];
		if (acc > target) { bin = b; break; }
	}
	return lo + ((float)bin + 0.5f) * (float)(1.0 / scale);
}

/* Multi-percentile build: one histogram answers several queries. */
#define PCTL_BINS 4096
typedef struct {
	float lo, hi;
	size_t n;
	double binToVal;
	unsigned int hist[PCTL_BINS];
} Histo;

static void histoBuild(const float *buf, size_t n, Histo *H)
{
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (size_t i = 0; i < n; i++) {
		float v = buf[i];
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	if (!(hi > lo)) hi = lo + 1e-6f;
	H->lo = lo; H->hi = hi; H->n = n;
	H->binToVal = (double)(hi - lo) / (double)(PCTL_BINS - 1);
	double scale = 1.0 / H->binToVal;
	memset(H->hist, 0, sizeof H->hist);
	for (size_t i = 0; i < n; i++) {
		int b = (int)((double)(buf[i] - lo) * scale);
		H->hist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)]++;
	}
}

static float histoPctl(const Histo *H, double p)
{
	size_t target = (size_t)(p * (double)H->n);
	size_t acc = 0;
	int bin = PCTL_BINS - 1;
	for (int b = 0; b < PCTL_BINS; b++) {
		acc += H->hist[b];
		if (acc > target) { bin = b; break; }
	}
	return H->lo + ((float)bin + 0.5f) * (float)H->binToVal;
}

/* Bilinear height: S wraps across the dateline, T clamps at the poles. */
static inline float hAt(const float *h, int W, int H, float x, float y)
{
	if (x < 0.0f) x += (float)W;
	if (x >= (float)W) x -= (float)W;
	if (x < 0.0f) x = 0.0f;
	if (x > (float)(W - 1)) x = (float)(W - 1);
	if (y < 0.0f) y = 0.0f;
	if (y > (float)(H - 1)) y = (float)(H - 1);
	int x0 = (int)x, y0 = (int)y;
	int x1 = x0 + 1; if (x1 >= W) x1 = 0;
	int y1 = y0 + 1; if (y1 >= H) y1 = H - 1;
	float fx = x - (float)x0, fy = y - (float)y0;
	const float *r0 = h + (size_t)y0 * W;
	const float *r1 = h + (size_t)y1 * W;
	float a = r0[x0] + (r0[x1] - r0[x0]) * fx;
	float b = r1[x0] + (r1[x1] - r1[x0]) * fx;
	return a + (b - a) * fy;
}

/* Weathering: gravity-driven talus relaxation, mass-conserving. For each
   texel and each lower 4-neighbor (so each pair acts once, from the higher
   side) half of the over-talus difference transfers immediately (Gauss-
   Seidel). NOTE: deferring the moves into a per-pass delta buffer looks
   equivalent but is not: a texel relaxing against 4 lower neighbors at once
   removes 2x its excess, overshooting the talus and amplifying ~1.5x per
   pass — the field explodes exponentially. Immediate transfers always damp.
   S wraps, T clamps. */
static void thermalErode(float *h, int W, int H, int passes, float talus,
                         void (*step)(void))
{
	int stepEvery = passes / TERRAIN_THERMAL_STEPS;
	if (stepEvery < 1) stepEvery = 1;
	int stepsTaken = 0;
	for (int p = 0; p < passes; p++) {
		if (step && stepsTaken < TERRAIN_THERMAL_STEPS && p % stepEvery == 0) {
			step();
			stepsTaken++;
		}
		for (int y = 0; y < H; y++) {
			size_t row = (size_t)y * W;
			for (int x = 0; x < W; x++) {
				size_t i = row + x;
				float hc = h[i];
				#define TPAIR(j) { \
						float hn = h[j]; \
						if (hn < hc) { \
							float excess = hc - hn - talus; \
							if (excess > 0.0f) { \
								float mv = 0.5f * excess; \
								hc -= mv; h[j] += mv; \
							} \
						} }
				TPAIR(row + (x > 0 ? x - 1 : W - 1));
				TPAIR(row + (x < W - 1 ? x + 1 : 0));
				if (y > 0) TPAIR(row - W + x);
				if (y < H - 1) TPAIR(row + W + x);
				h[i] = hc;
			}
		}
	}
}

/* Fixed-seed PRNG (xorshift32); never zero after a nonzero seed. */
static unsigned int xsState;
static inline float xsRand(void)
{
	xsState ^= xsState << 13;
	xsState ^= xsState >> 17;
	xsState ^= xsState << 5;
	return (float)(xsState >> 8) * (1.0f / 16777216.0f);
}

/* Hydraulic erosion, droplet method (Mei et al. 2007 / kaylut): droplets
   spawn only above the pre-erosion sea level (ocean-starting droplets
   otherwise carve the seafloor and wreck the depth stats), advect downhill
   with inertia, erode up to their transport capacity and deposit the
   surplus. Erosion never digs below the pre-erosion field (min-clamp);
   deposition is bilinear, so the whole pass conserves sediment mass. The
   hot loop tracks the droplet's bilinear 2x2 cell across steps so each step
   is a handful of cached loads instead of four clamped resamples. */
static void hydraulicErode(float *h, const float *pre, int W, int H, float seaLevel0,
                           void (*step)(void))
{
	size_t n = (size_t)W * H;
	double dropsD = (double)DROPLETS_PER_PX * (double)n;
	long drops = (long)(dropsD > 9.0e18 ? 9.0e18 : dropsD);
	const float evapKeep = 1.0f - DROPLET_EVAP_RATE;
	long stepEvery = drops / TERRAIN_HYDRO_STEPS;
	if (stepEvery < 1) stepEvery = 1;
	int stepsTaken = 0;
	for (long d = 0; d < drops; d++) {
		if (step && stepsTaken < TERRAIN_HYDRO_STEPS && d % stepEvery == 0) {
			step();
			stepsTaken++;
		}
		size_t i = 0;
		int found = 0;
		for (int tries = 0; tries < 24; tries++) {
			i = (size_t)(xsRand() * (float)n);
			if (i >= n) i = n - 1;
			if (h[i] > seaLevel0) { found = 1; break; }
		}
		if (!found) continue;

		float x = (float)(unsigned int)(i % (unsigned int)W);
		float y = (float)(i / (unsigned int)W);
		int x0 = (int)x, y0 = (int)y;
		float fx = 0.0f, fy = 0.0f;
		int x1 = x0 + 1; if (x1 == W) x1 = 0;
		int y1 = y0 + 1; if (y1 == H) y1 = H - 1;
		const float *r0 = h + (size_t)y0 * W;
		const float *r1 = h + (size_t)y1 * W;
		float hCur = r0[x0];

		float dx = 0.0f, dy = 0.0f;
		float speed = 1.0f;
		float water = DROPLET_START_WATER;
		float sediment = 0.0f;

		for (int step = 0; step < DROPLET_MAX_STEPS && water > DROPLET_MIN_WATER; step++) {
			/* Downhill gradient straight from the bilinear 2x2 cell. */
			float gx = ((r0[x1] - r0[x0]) + (r1[x1] - r1[x0])) * 0.25f;
			float gy = ((r1[x0] - r0[x0]) + (r1[x1] - r0[x1])) * 0.25f;
			float ndx = dx * DROPLET_INERTIA - gx * (1.0f - DROPLET_INERTIA);
			float ndy = dy * DROPLET_INERTIA - gy * (1.0f - DROPLET_INERTIA);
			float len = sqrtf(ndx * ndx + ndy * ndy);
			if (len < 1e-7f) break;           /* stalled/flat: kill, never divide */
			ndx /= len; ndy /= len;
			dx = ndx; dy = ndy;

			float nx = x + dx * DROPLET_STEP;
			float ny = y + dy * DROPLET_STEP;
			if (nx < 0.0f) nx += (float)W; else if (nx >= (float)W) nx -= (float)W;
			if (ny < 0.0f || ny > (float)(H - 2)) break;  /* reached a pole */

			int nx0 = (int)nx, ny0 = (int)ny;
			int nx1 = nx0 + 1; if (nx1 == W) nx1 = 0;
			int ny1 = ny0 + 1; if (ny1 == H) ny1 = H - 1;
			float nfx = nx - (float)nx0, nfy = ny - (float)ny0;
			const float *s0 = h + (size_t)ny0 * W;
			const float *s1 = h + (size_t)ny1 * W;
			float ha = s0[nx0] + (s0[nx1] - s0[nx0]) * nfx;
			float hb = s1[nx0] + (s1[nx1] - s1[nx0]) * nfx;
			float hNew = ha + (hb - ha) * nfy;

			float dh = hNew - hCur;
			speed = sqrtf(speed * speed + fabsf(dh) * DROPLET_GRAVITY);
			float capacity = fmaxf(-dh, 0.0f) * speed * water * DROPLET_SEDIMENT_MAX;

			float wx0 = 1.0f - fx, wx1 = fx, wy0 = 1.0f - fy, wy1 = fy;
			if (sediment > capacity) {
				float amt = (sediment - capacity) * DROPLET_DEPOSIT_RATE;
				h[(size_t)y0 * W + x0] += amt * wx0 * wy0;
				h[(size_t)y0 * W + x1] += amt * wx1 * wy0;
				h[(size_t)y1 * W + x0] += amt * wx0 * wy1;
				h[(size_t)y1 * W + x1] += amt * wx1 * wy1;
				sediment -= amt;
			} else {
				float diff = capacity - sediment;
				float amt = fminf(diff, capacity) * DROPLET_EROSION_RATE;
				float minH = hAt(pre, W, H, x, y);
				if (hCur - amt < minH) amt = fmaxf(hCur - minH, 0.0f);
				if (amt > 0.0f) {
					h[(size_t)y0 * W + x0] -= amt * wx0 * wy0;
					h[(size_t)y0 * W + x1] -= amt * wx1 * wy0;
					h[(size_t)y1 * W + x0] -= amt * wx0 * wy1;
					h[(size_t)y1 * W + x1] -= amt * wx1 * wy1;
					sediment += amt;
				}
			}
			water *= evapKeep;
			x = nx; y = ny;
			x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
			fx = nfx; fy = nfy;
			r0 = s0; r1 = s1;
			hCur = hNew;
		}
	}
}

/* ---------------- GPU shaping (cached across re-bakes) ---------------- */

static unsigned int bakeProgram, bakeQuadVAO, workFBO, workTex;

static void ensureBakeResources(void)
{
	if (bakeProgram) return;

	createShader(&bakeProgram, "shaders/heightbake.vert", "shaders/heightbake.frag");

	static const float quadCoords[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	glGenVertexArrays(1, &bakeQuadVAO);
	glBindVertexArray(bakeQuadVAO);
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadCoords), quadCoords, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	/* Working RGBA16F target + GL_RGBA/GL_FLOAT readback: the max-compat
	   combination for a float shaping pass. */
	glGenTextures(1, &workTex);
	glBindTexture(GL_TEXTURE_2D, workTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, HEIGHTMAP_W, HEIGHTMAP_H, 0,
	             GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &workFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, workTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[terrain] working framebuffer incomplete: 0x%x\n",
		        (unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER));
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ---------------- Full bake ---------------- */

unsigned int generatePlanetHeightmap(unsigned int seed, PlanetGenParams *out,
                                     void (*step)(void))
{
	const int W = HEIGHTMAP_W, H = HEIGHTMAP_H;
	const size_t n = (size_t)W * H;

	ensureBakeResources();

	clock_t t0 = clock();

	GLuint heightTex;
	glGenTextures(1, &heightTex);

	xsState = seed * 2654435761u | 1u;

	/* 1. GPU shaping pass */
	glViewport(0, 0, W, H);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glUseProgram(bakeProgram);
	glUniform1f(glGetUniformLocation(bakeProgram, "seed"), (float)seed);
	glBindVertexArray(bakeQuadVAO);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	clock_t t1 = clock();

	/* 2. Readback to a CPU float buffer (r channel holds the shaped h). */
	float *rgba = malloc(n * 4 * sizeof(float));
	float *h = malloc(n * sizeof(float));
	if (!rgba || !h)
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	for (size_t i = 0; i < n; i++) h[i] = rgba[i * 4];
	free(rgba);
	clampFinite(h, n);
	clock_t t2 = clock();

	/* 3. Thermal erosion (weathering), then 4. hydraulic droplet erosion. */
	thermalErode(h, W, H, THERMAL_PASSES, THERMAL_TALUS, step);
	clock_t t3 = clock();

	/* Coarse pre-erosion sea level: spawn droplets only on land, never in
	   the ocean basins. */
	float seaLevel0 = histPercentile(h, n, WATER_COVERAGE);
	float *pre = malloc(n * sizeof(float));
	memcpy(pre, h, n * sizeof(float));
	hydraulicErode(h, pre, W, H, seaLevel0, step);
	free(pre);
	clock_t t4 = clock();

	/* 5. Percentile-stretch the relief to [0,1] and derive the sea level
	   (constant giving ~WATER_COVERAGE water area) + ocean depth (95th
	   percentile of the depth below sea level, feeds the water.frag tint). */
	Histo hf;
	histoBuild(h, n, &hf);
	float lo = histoPctl(&hf, NORMALIZE_LO_PCT);
	float hi = histoPctl(&hf, NORMALIZE_HI_PCT);
	float range = hi - lo;
	if (range < 1e-6f) range = 1.0f;
	float invRange = 1.0f / range;
	float seaLevel = (histoPctl(&hf, WATER_COVERAGE) - lo) * invRange;
	seaLevel = seaLevel < 0.0f ? 0.0f : (seaLevel > 1.0f ? 1.0f : seaLevel);
	for (size_t i = 0; i < n; i++) {
		float v = (h[i] - lo) * invRange;
		if (!(v == v)) v = 0.0f;
		h[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}

	/* Depth histogram over the ocean texels only: depths lie in (0, seaLevel]. */
	size_t oceanCount = 0;
	static unsigned int dhist[PCTL_BINS];
	memset(dhist, 0, sizeof dhist);
	float oceanDepth = 0.15f;
	if (seaLevel > 1e-6f) {
		double dscale = (double)(PCTL_BINS - 1) / (double)seaLevel;
		for (size_t i = 0; i < n; i++) {
			if (h[i] < seaLevel) {
				oceanCount++;
				int b = (int)((double)(seaLevel - h[i]) * dscale);
				dhist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)]++;
			}
		}
		if (oceanCount > 0) {
			size_t target = (size_t)(OCEAN_DEPTH_PCT * (double)oceanCount);
			size_t acc = 0;
			int bin = PCTL_BINS - 1;
			for (int b = 0; b < PCTL_BINS; b++) {
				acc += dhist[b];
				if (acc > target) { bin = b; break; }
			}
			oceanDepth = ((float)bin + 0.5f) / (float)dscale;
		}
	}
	if (!(oceanDepth > 1e-3f)) oceanDepth = 1e-3f;

	out->seaLevel = seaLevel;
	out->oceanDepth = oceanDepth;
	g_planetGen = *out;
	clock_t t5 = clock();

	/* 6. Upload the final R16F field (same sampling params the body shaders
	   expect) + a full mip chain. */
	glBindTexture(GL_TEXTURE_2D, heightTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, W, H, 0, GL_RED, GL_FLOAT, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	/* Longitude is periodic; the latitude clamp keeps the poles from
	   wrapping into each other. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(h);

	glViewport(0, 0, getWindowWidth(), getWindowHeight());
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);

	double s = (double)(CLOCKS_PER_SEC);
	fprintf(stderr, "[terrain] bake seed %u: shape %.2f s, readback %.2f s, thermal %.2f s, hydro %.2f s, sea %.2f s -> seaLevel %.4f, oceanDepth %.4f\n",
	        seed,
	        (double)(t1 - t0) / s, (double)(t2 - t1) / s, (double)(t3 - t2) / s,
	        (double)(t4 - t3) / s, (double)(t5 - t4) / s, seaLevel, oceanDepth);

	return heightTex;
}
