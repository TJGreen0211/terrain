#include "Main.h"
#include "TerrainGen.h"
#include "WorldConfig.h"
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
PlanetGenParams g_planetGen = { 0.5f, 0.22f, 0 };

#define HEIGHTMAP_W 2048
#define HEIGHTMAP_H 1024

/* ---- Bake pipeline (tweak here; shaping recipe constants live in
   shaders/heightbake.frag). Erosion runs on the field pre-normalized to
   [0,1] (p1..p99 stretch), so every height constant below is in those
   units and independent of the shader's raw output scale. ---- */
#define PRE_LO_PCT           0.01    /* pre-erosion percentile stretch */
#define PRE_HI_PCT           0.999   /* high: mountain belts are a small share of the area */

/* Thermal passes/talus, droplet count and rates: g_terrainBake (below). */
#define DROPLET_MAX_STEPS    64      /* lifetime, in 1-texel steps */
#define DROPLET_BRUSH_RADIUS 2       /* erosion footprint radius (texels) */
#define DROPLET_GRAVITY      4.0f
#define DROPLET_MIN_SLOPE    0.0005f /* capacity floor on near-flat ground */
#define DROPLET_START_SPEED  1.0f
#define DROPLET_START_WATER  1.0f
#define DELTA_DEPTH          0.004f  /* river-mouth deposits stay this far under sea level */

#define NORMALIZE_LO_PCT     0.01    /* final percentile stretch of the relief */
#define NORMALIZE_HI_PCT     0.999
#define OCEAN_DEPTH_PCT      0.95    /* depth normalizing percentile */

/* ---- Tectonic plates (shaping input for heightbake.frag) ---- */
/* Plate count and continental area: g_terrainBake (below). */
#define PLATE_MIN_SEP        0.45    /* min angle between plate centres (rad) */
#define PLATE_MARGIN_MAX     0.10f   /* passive-margin pullback range (rad) */

/* Bake inputs (TerrainGen.h). Shaping defaults mirror the [default]
   notes in heightbake.frag. */
TerrainBakeSettings g_terrainBake = {
	.seed = 1337u,
	.plateCount = 14,
	.continentArea = 0.42f,
	.coastFreq = 2.2f,
	.coastAmt = 0.12f,
	.plainsH = 0.06f,
	.hillAmt = 0.10f,
	.plateauH = 0.30f,
	.collideH = 1.30f,
	.andesH = 0.95f,
	.oldRangeH = 0.35f,
	.oceanBase = -0.70f,
	.ridgeH = 0.28f,
	.hotspotLo = 0.50f,
	.fragLo = 0.42f,
	.detailAmt = 0.06f,
	.thermalPasses = 6,
	.thermalTalus = 0.006f,    /* only the steepest crests/cliffs exceed it */
	.dropletsPerPx = 0.35f,
	.dropletErosion = 0.3f,
	.dropletDeposit = 0.3f,
	.dropletCapacity = 4.0f,
	.dropletEvap = 0.02f,
	.dropletInertia = 0.05f,
};

/* ---- Climate map (moisture / rivers / lakes); rates are per physical
   texel (one equator texel of distance) ---- */
#define WIND_TRADE_LAT       30.0    /* trade winds (east->west) below this */
#define WIND_POLAR_LAT       60.0    /* westerlies up to this, polar easterlies above */
#define OCEAN_RECHARGE       0.06f   /* humidity regained per texel over sea */
#define RAIN_BASE            0.004f  /* humidity rained out per texel over land */
#define RAIN_OROGRAPHIC      8.0f    /* extra rain-out per unit of uphill slope */
#define RAIN_RECYCLE         0.6f    /* fraction of rain re-evaporated (inland reach) */
#define MOIST_BLUR           4       /* smoothing radius, texels */
#define MOIST_NORM_PCT       0.60    /* land rain at this percentile maps to 1 */
#define ANOMALY_AMT          0.4f    /* weight of the regional wet/dry anomaly */
#define RIVER_RAIN_FLOOR     0.15f   /* deserts still feed rivers a little */
#define RIVER_LOG_MAX        16.0f   /* log2 drainage that maps to river = 1 */
#define RIVER_WIDEN          0.20f   /* river value lost per texel from the bed */
#define LAKE_DEPTH_NORM      0.05f   /* lake depth that maps to A = 1 */

/* Equirect texels shrink toward the poles: x-neighbours are cos(lat) texels
   apart on the sphere. Floor keeps the two polar rows finite. */
#define MIN_COS_LAT          0.02f

/* ---------------- CPU height field helpers ---------------- */

/* Replace NaN/Inf so no poison reaches histograms or the mip chain. */
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

static float rowCos[HEIGHTMAP_H];

/* cos(latitude) of every texel row's centre: the row's area weight. */
static void initRowCos(int H)
{
	for (int y = 0; y < H; y++) {
		double lat = (((double)y + 0.5) / (double)H - 0.5) * M_PI;
		float c = (float)cos(lat);
		rowCos[y] = c < MIN_COS_LAT ? MIN_COS_LAT : c;
	}
}

/* Area-weighted percentiles via histogram (no sort): each texel counts with
   its row's cos(lat), so the oversampled polar rows don't skew the stats.
   With 4096 bins over a smooth distribution the error is well below one
   texel's height step, and it keeps the 2M-value percentiles cheap (MSVC
   qsort measured ~1.7 s). One build answers several queries. */
#define PCTL_BINS 4096
typedef struct {
	float lo, hi;
	double total;
	double binToVal;
	double hist[PCTL_BINS];
} Histo;

static void histoBuild(const float *buf, int W, int H, Histo *Hs)
{
	size_t n = (size_t)W * H;
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (size_t i = 0; i < n; i++) {
		float v = buf[i];
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	if (!(hi > lo)) hi = lo + 1e-6f;
	Hs->lo = lo; Hs->hi = hi; Hs->total = 0.0;
	Hs->binToVal = (double)(hi - lo) / (double)(PCTL_BINS - 1);
	double scale = 1.0 / Hs->binToVal;
	memset(Hs->hist, 0, sizeof Hs->hist);
	for (int y = 0; y < H; y++) {
		const float *row = buf + (size_t)y * W;
		double w = rowCos[y];
		for (int x = 0; x < W; x++) {
			int b = (int)((double)(row[x] - lo) * scale);
			Hs->hist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)] += w;
		}
		Hs->total += w * W;
	}
}

static float histoPctl(const Histo *Hs, double p)
{
	double target = p * Hs->total;
	double acc = 0.0;
	int bin = PCTL_BINS - 1;
	for (int b = 0; b < PCTL_BINS; b++) {
		acc += Hs->hist[b];
		if (acc > target) { bin = b; break; }
	}
	return Hs->lo + ((float)bin + 0.5f) * (float)Hs->binToVal;
}

/* Linear stretch of [lo,hi] onto [0,1], clamped. */
static void stretch01(float *h, size_t n, float lo, float hi)
{
	float range = hi - lo;
	if (range < 1e-6f) range = 1.0f;
	float inv = 1.0f / range;
	for (size_t i = 0; i < n; i++) {
		float v = (h[i] - lo) * inv;
		if (!(v == v)) v = 0.0f;
		h[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}
}

/* Weathering: gravity-driven talus relaxation, mass-conserving. For each
   texel and each lower 4-neighbor (so each pair acts once, from the higher
   side) half of the over-talus difference transfers immediately (Gauss-
   Seidel). NOTE: deferring the moves into a per-pass delta buffer looks
   equivalent but is not: a texel relaxing against 4 lower neighbors at once
   removes 2x its excess, overshooting the talus and amplifying ~1.5x per
   pass — the field explodes exponentially. Immediate transfers always damp.
   Gauss-Seidel drags material along the sweep direction, so odd passes run
   the sweep backwards to cancel the bias. The east/west talus is scaled by
   cos(lat) (those neighbours are physically closer), so the poles don't
   over-erode. S wraps, T clamps. */
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
		int rev = p & 1;
		for (int yy = 0; yy < H; yy++) {
			int y = rev ? H - 1 - yy : yy;
			size_t row = (size_t)y * W;
			float talusX = talus * rowCos[y];
			for (int xx = 0; xx < W; xx++) {
				int x = rev ? W - 1 - xx : xx;
				size_t i = row + x;
				float hc = h[i];
				#define TPAIR(j, t) { \
						float hn = h[j]; \
						if (hn < hc) { \
							float excess = hc - hn - (t); \
							if (excess > 0.0f) { \
								float mv = 0.5f * excess; \
								hc -= mv; h[j] += mv; \
							} \
						} }
				TPAIR(row + (x > 0 ? x - 1 : W - 1), talusX);
				TPAIR(row + (x < W - 1 ? x + 1 : 0), talusX);
				if (y > 0) TPAIR(row - W + x, talus);
				if (y < H - 1) TPAIR(row + W + x, talus);
				#undef TPAIR
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

/* Erosion brush: texel offsets within DROPLET_BRUSH_RADIUS, weights falling
   off linearly with distance and summing to 1. */
#define BRUSH_MAX ((2 * DROPLET_BRUSH_RADIUS + 1) * (2 * DROPLET_BRUSH_RADIUS + 1))
static int brushN;
static int brushDX[BRUSH_MAX], brushDY[BRUSH_MAX];
static float brushW[BRUSH_MAX];

static void initBrush(void)
{
	const int r = DROPLET_BRUSH_RADIUS;
	float sum = 0.0f;
	brushN = 0;
	for (int dy = -r; dy <= r; dy++)
		for (int dx = -r; dx <= r; dx++) {
			float w = (float)r - sqrtf((float)(dx * dx + dy * dy));
			if (w <= 0.0f) continue;
			brushDX[brushN] = dx; brushDY[brushN] = dy; brushW[brushN] = w;
			sum += w;
			brushN++;
		}
	for (int k = 0; k < brushN; k++) brushW[k] /= sum;
}

/* Bilinear height + gradient at a texel position, x wrapping across the
   dateline. Caller keeps y in [0, H-2]. */
static inline float sampleHG(const float *h, int W, float x, float y, float *gx, float *gy)
{
	int x0 = (int)x, y0 = (int)y;
	int x1 = x0 + 1; if (x1 >= W) x1 = 0;
	float fx = x - (float)x0, fy = y - (float)y0;
	const float *r0 = h + (size_t)y0 * W;
	const float *r1 = r0 + W;
	float h00 = r0[x0], h10 = r0[x1], h01 = r1[x0], h11 = r1[x1];
	*gx = (h10 - h00) * (1.0f - fy) + (h11 - h01) * fy;
	*gy = (h01 - h00) * (1.0f - fx) + (h11 - h10) * fx;
	return h00 * (1.0f - fx) * (1.0f - fy) + h10 * fx * (1.0f - fy)
	     + h01 * (1.0f - fx) * fy + h11 * fx * fy;
}

/* Spread amt over the brush centred on texel (cx, cy): positive deposits
   (never raising a texel above ceil), negative erodes (never below zero).
   Returns the amount actually moved (signed like amt). */
static inline float applyBrush(float *h, int W, int H, int cx, int cy, float amt,
                               float ceil)
{
	float moved = 0.0f;
	for (int k = 0; k < brushN; k++) {
		int yy = cy + brushDY[k];
		if (yy < 0 || yy >= H) continue;
		int xx = cx + brushDX[k];
		if (xx < 0) xx += W; else if (xx >= W) xx -= W;
		float *t = &h[(size_t)yy * W + xx];
		float d = amt * brushW[k];
		if (d < 0.0f && -d > *t) d = -*t;
		if (d > 0.0f && *t + d > ceil) d = fmaxf(ceil - *t, 0.0f);
		*t += d;
		moved += d;
	}
	return moved;
}

/* Hydraulic erosion, droplet method (Mei et al. 2007 / Lague): droplets
   spawn on land only (above the pre-erosion sea level) with an area-uniform
   density (row rejection by cos(lat)), move one physical texel per step
   downhill with inertia, erode with a radial brush up to their transport
   capacity (never more than the height they just dropped, so they can't dig
   pits) and deposit the surplus. Uphill steps fill the pit behind them.
   A droplet that reaches the sea drops its load there (river deltas) and
   dies, so the seafloor is never carved. Equirect correction: the x
   gradient and x motion are divided by cos(lat) so droplets travel
   isotropically on the sphere. Every on-land deposit is also summed into
   dep (alluvial plains for the climate map). */
static void hydraulicErode(float *h, float *dep, int W, int H, float seaLevel0,
                           const TerrainBakeSettings *bs, void (*step)(void))
{
	size_t n = (size_t)W * H;
	long drops = (long)((double)bs->dropletsPerPx * (double)n);
	const float evapKeep = 1.0f - bs->dropletEvap;
	const float inertia = bs->dropletInertia;
	long stepEvery = drops / TERRAIN_HYDRO_STEPS;
	if (stepEvery < 1) stepEvery = 1;
	int stepsTaken = 0;
	for (long d = 0; d < drops; d++) {
		if (step && stepsTaken < TERRAIN_HYDRO_STEPS && d % stepEvery == 0) {
			step();
			stepsTaken++;
		}
		/* Spawn: random land texel, accepted with probability cos(lat). */
		float x = 0.0f, y = 0.0f;
		int found = 0;
		for (int tries = 0; tries < 32; tries++) {
			x = xsRand() * (float)W;
			y = xsRand() * (float)(H - 1);
			if (x >= (float)W) x = 0.0f;
			int iy = (int)y;
			if (xsRand() > rowCos[iy]) continue;
			if (h[(size_t)iy * W + (int)x] > seaLevel0) { found = 1; break; }
		}
		if (!found) continue;

		float dx = 0.0f, dy = 0.0f;
		float speed = DROPLET_START_SPEED;
		float water = DROPLET_START_WATER;
		float sediment = 0.0f;
		float gx, gy;
		float hCur = sampleHG(h, W, x, y, &gx, &gy);

		for (int s = 0; s < DROPLET_MAX_STEPS; s++) {
			int cx = (int)x, cy = (int)y;
			float invCos = 1.0f / rowCos[cy];

			float ndx = dx * inertia - gx * invCos * (1.0f - inertia);
			float ndy = dy * inertia - gy * (1.0f - inertia);
			float len = sqrtf(ndx * ndx + ndy * ndy);
			if (len < 1e-9f) break;           /* stalled/flat: kill, never divide */
			dx = ndx / len; dy = ndy / len;

			float nx = x + dx * invCos;
			float ny = y + dy;
			if (nx < 0.0f) nx += (float)W; else if (nx >= (float)W) nx -= (float)W;
			if (nx >= (float)W) nx = 0.0f;
			if (ny < 0.0f || ny >= (float)(H - 1)) break;  /* reached a pole */

			float ngx, ngy;
			float hNew = sampleHG(h, W, nx, ny, &ngx, &ngy);
			float dh = hNew - hCur;

			if (hNew < seaLevel0) {
				/* River mouth: drop the load as shallows and stop. Capped
				   just under sea level so deltas never surface as one-texel
				   specks along the coast; the rest is carried out to sea. */
				if (sediment > 0.0f)
					applyBrush(h, W, H, (int)nx, (int)ny, sediment,
					           seaLevel0 - DELTA_DEPTH);
				break;
			}

			float capacity = fmaxf(-dh, DROPLET_MIN_SLOPE) * speed * water * bs->dropletCapacity;
			if (sediment > capacity || dh > 0.0f) {
				float amt = (dh > 0.0f) ? fminf(dh, sediment)
				                        : (sediment - capacity) * bs->dropletDeposit;
				if (amt > 0.0f) {
					/* Bilinear deposit at the droplet's own position. */
					float fx = x - (float)cx, fy = y - (float)cy;
					int cx1 = cx + 1; if (cx1 >= W) cx1 = 0;
					float *r0 = h + (size_t)cy * W;
					float *r1 = r0 + W;
					r0[cx]  += amt * (1.0f - fx) * (1.0f - fy);
					r0[cx1] += amt * fx * (1.0f - fy);
					r1[cx]  += amt * (1.0f - fx) * fy;
					r1[cx1] += amt * fx * fy;
					dep[(size_t)cy * W + cx] += amt;
					sediment -= amt;
				}
			} else {
				float amt = fminf((capacity - sediment) * bs->dropletErosion, -dh);
				if (amt > 0.0f)
					sediment -= applyBrush(h, W, H, cx, cy, -amt, FLT_MAX);
			}

			speed = sqrtf(fmaxf(speed * speed - dh * DROPLET_GRAVITY, 0.0f));
			water *= evapKeep;
			/* Carry the new sample forward (the brush may have nudged it
			   slightly; re-sampling costs a third of the loop for no
			   visible gain). */
			x = nx; y = ny;
			hCur = hNew; gx = ngx; gy = ngy;
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

	initRowCos(HEIGHTMAP_H);
	initBrush();
}

/* ---------------- Climate map ---------------- */

/* Separable box blur, x radius scaled by 1/cos(lat) so the footprint is
   round on the sphere; x wraps, y clamps. Two passes ~ a soft tent. */
static void blurField(float *f, int W, int H, int r)
{
	float *tmp = malloc((size_t)(W > H ? W : H) * sizeof(float));
	double *pre = malloc((size_t)(3 * W + 1) * sizeof(double));
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < H; y++) {
			float *row = f + (size_t)y * W;
			int rx = (int)((float)r / rowCos[y]);
			if (rx > W / 4) rx = W / 4;
			if (rx < 1) continue;
			/* Prefix sums over three copies of the row: window [x-rx, x+rx]
			   maps to [x-rx+W, x+rx+W], always inside [0, 3W). */
			pre[0] = 0.0;
			for (int k = 0; k < 3 * W; k++) pre[k + 1] = pre[k] + row[k % W];
			double inv = 1.0 / (double)(2 * rx + 1);
			for (int x = 0; x < W; x++)
				tmp[x] = (float)((pre[x + rx + W + 1] - pre[x - rx + W]) * inv);
			memcpy(row, tmp, (size_t)W * sizeof(float));
		}
		for (int x = 0; x < W; x++) {
			double acc = 0.0;
			int cnt = 0;
			for (int y = 0; y <= r && y < H; y++) { acc += f[(size_t)y * W + x]; cnt++; }
			for (int y = 0; y < H; y++) {
				tmp[y] = (float)(acc / cnt);
				int add = y + r + 1, rem = y - r;
				if (add < H) { acc += f[(size_t)add * W + x]; cnt++; }
				if (rem >= 0) { acc -= f[(size_t)rem * W + x]; cnt--; }
			}
			for (int y = 0; y < H; y++) f[(size_t)y * W + x] = tmp[y];
		}
	}
	free(pre);
	free(tmp);
}

/* Moisture: each latitude row is one stream of the zonal prevailing wind
   (trades blow east->west, westerlies west->east, polar easterlies
   east->west). Humidity recharges over sea, rains out slowly over land
   (continental interiors dry) and fast on rising ground (orographic rain:
   wet windward slopes, rain shadow behind ranges). Two laps around the
   row converge the wrap. The rain density is smoothed, normalized against
   the land's MOIST_NORM_PCT percentile and nudged by the regional anomaly
   noise so the climate isn't purely zonal. The latitude rain belts
   (equatorial ITCZ, subtropical deserts) are applied live in quad.frag so
   they stay tunable per planet. */
static void computeMoisture(const float *h, const float *anom, float sea,
                            int W, int H, float *m)
{
	for (int y = 0; y < H; y++) {
		const float *row = h + (size_t)y * W;
		float *out = m + (size_t)y * W;
		double latDeg = fabs((((double)y + 0.5) / (double)H - 0.5) * 180.0);
		int dir = (latDeg < WIND_TRADE_LAT || latDeg >= WIND_POLAR_LAT) ? -1 : 1;
		float c = rowCos[y];
		float recharge = 1.0f - expf(-OCEAN_RECHARGE * c);
		float q = 1.0f;
		for (int lap = 0; lap < 2; lap++) {
			for (int k = 0; k < W; k++) {
				int x = dir > 0 ? k : W - 1 - k;
				int xn = x + dir; if (xn < 0) xn += W; else if (xn >= W) xn -= W;
				float hc = row[x];
				if (hc < sea) {
					q += (1.0f - q) * recharge;
					if (lap) out[x] = q * RAIN_BASE;   /* coastal air: wet */
					continue;
				}
				float rise = fmaxf(row[xn] - hc, 0.0f) / c;
				float rate = RAIN_BASE + RAIN_OROGRAPHIC * rise;
				float rain = q * (1.0f - expf(-rate * c));
				q -= rain * (1.0f - RAIN_RECYCLE);
				if (lap) out[x] = rain / c;
			}
		}
		/* sqrt compresses the range: orographic spikes on windward coasts
		   would otherwise set the normalization and leave interiors at 0. */
		for (int x = 0; x < W; x++) out[x] = sqrtf(out[x]);
	}
	blurField(m, W, H, MOIST_BLUR);

	/* Normalize on land only (ocean texels hold coastal humidity): the
	   land values sit above the ocean's zeros in this histogram, so the
	   percentile is rescaled into the land's share of the area. */
	static Histo mh;
	size_t n = (size_t)W * H;
	float *landVals = malloc(n * sizeof(float));
	for (size_t i = 0; i < n; i++) landVals[i] = h[i] >= sea ? m[i] : 0.0f;
	histoBuild(landVals, W, H, &mh);
	free(landVals);
	double landArea = 0.0, allArea = 0.0;
	for (int y = 0; y < H; y++) {
		const float *row = h + (size_t)y * W;
		for (int x = 0; x < W; x++) if (row[x] >= sea) landArea += rowCos[y];
		allArea += (double)rowCos[y] * W;
	}
	double landFrac = allArea > 0.0 ? landArea / allArea : 0.5;
	float pNorm = histoPctl(&mh, 1.0 - (1.0 - MOIST_NORM_PCT) * landFrac);
	if (!(pNorm > 1e-9f)) pNorm = 1e-9f;
	/* Stored at half scale (quad.frag doubles it): windward coasts can rain
	   up to 2x the percentile and keep that headroom through the latitude
	   belt multiply. */
	for (size_t i = 0; i < n; i++) {
		float v = 0.5f * (m[i] / pNorm + ANOMALY_AMT * anom[i]);
		m[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}
}

/* quad.frag's hadleyFactor (latitude rain belts), mirrored so river
   discharge follows the same climate the surface is shaded with. Uses the
   style at bake time; re-bake (H) after changing it to update rivers. */
static float hadleyFactorC(double absLatDeg)
{
	const PlanetStyle *ps = &g_planetStyle;
	double itcz    = exp(-pow(absLatDeg / 14.0, 2.0));
	double subtrop = exp(-pow((absLatDeg - 27.0) / 9.0, 2.0));
	double midlat  = exp(-pow((absLatDeg - 55.0) / 12.0, 2.0));
	double f = 0.65 + 0.75 * ps->equatorLush * itcz - 0.35 * subtrop + 0.2 * midlat;
	return (float)(1.0 + (f - 1.0) * ps->hadleyStrength) * ps->humidity;
}

/* Min-heap for the priority flood. */
typedef struct { float h; int i; } PQNode;
static PQNode *pq;
static int pqN;

static inline void pqPush(float h, int i)
{
	int k = pqN++;
	while (k > 0) {
		int p = (k - 1) >> 1;
		if (pq[p].h <= h) break;
		pq[k] = pq[p];
		k = p;
	}
	pq[k].h = h; pq[k].i = i;
}

static inline PQNode pqPop(void)
{
	PQNode top = pq[0], last = pq[--pqN];
	int k = 0;
	for (;;) {
		int c = 2 * k + 1;
		if (c >= pqN) break;
		if (c + 1 < pqN && pq[c + 1].h < pq[c].h) c++;
		if (pq[c].h >= last.h) break;
		pq[k] = pq[c];
		k = c;
	}
	pq[k] = last;
	return top;
}

/* Drainage (Barnes et al. priority flood): flood inward from the coast
   over 8-neighbours in order of water level. Each land texel drains to the
   neighbour that reached it, so every texel gets a path to the sea, also
   through closed basins; the flood level above the ground there is a lake.
   The pop order is ascending water level, so walking it backwards visits
   upstream texels first: accumulating rain-weighted area along the drain
   tree gives each texel's discharge. River strength = log2 discharge,
   widened by RIVER_WIDEN per texel so big rivers survive the mip chain. */
static void computeDrainage(const float *h, const float *moist, float sea,
                            int W, int H, float *river, float *lake)
{
	size_t n = (size_t)W * H;
	float *filled = malloc(n * sizeof(float));
	int *recv = malloc(n * sizeof(int));
	int *order = malloc(n * sizeof(int));
	unsigned char *done = calloc(n, 1);
	pq = malloc(n * sizeof(PQNode));
	pqN = 0;
	static const int NDX[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
	static const int NDY[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };

	for (size_t i = 0; i < n; i++) {
		recv[i] = -1;
		if (h[i] < sea) { done[i] = 1; filled[i] = h[i]; }
	}
	/* Seeds: ocean texels touching land. */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			size_t i = (size_t)y * W + x;
			if (h[i] >= sea) continue;
			for (int k = 0; k < 8; k++) {
				int yy = y + NDY[k];
				if (yy < 0 || yy >= H) continue;
				int xx = (x + NDX[k] + W) % W;
				if (h[(size_t)yy * W + xx] >= sea) { pqPush(h[i], (int)i); break; }
			}
		}
	if (pqN == 0) {   /* no ocean at all: drain to the global minimum */
		size_t lo = 0;
		for (size_t i = 1; i < n; i++) if (h[i] < h[lo]) lo = i;
		done[lo] = 1; filled[lo] = h[lo];
		pqPush(h[lo], (int)lo);
	}

	size_t m = 0;
	while (pqN > 0) {
		PQNode c = pqPop();
		if (h[c.i] >= sea) order[m++] = c.i;
		int x = c.i % W, y = c.i / W;
		for (int k = 0; k < 8; k++) {
			int yy = y + NDY[k];
			if (yy < 0 || yy >= H) continue;
			int xx = (x + NDX[k] + W) % W;
			int j = yy * W + xx;
			if (done[j]) continue;
			done[j] = 1;
			filled[j] = fmaxf(h[j], c.h);
			recv[j] = c.i;
			pqPush(filled[j], j);
		}
	}
	free(pq); pq = NULL;
	free(done);

	/* Rain-weighted, area-weighted discharge, upstream first. */
	float *acc = river;   /* reuse the output buffer for the accumulation */
	for (int y = 0; y < H; y++) {
		double latDeg = fabs((((double)y + 0.5) / (double)H - 0.5) * 180.0);
		float rowRain = hadleyFactorC(latDeg) * rowCos[y];
		for (int x = 0; x < W; x++) {
			size_t i = (size_t)y * W + x;
			float r = 2.0f * moist[i] * rowRain;
			acc[i] = h[i] >= sea ? (RIVER_RAIN_FLOOR * rowCos[y] + (r > 0.0f ? r : 0.0f)) : 0.0f;
		}
	}
	for (size_t k = m; k-- > 0; ) {
		int c = order[k], r = recv[c];
		if (r >= 0 && h[r] >= sea) acc[r] += acc[c];
	}
	free(order);
	free(recv);

	float *L = malloc(n * sizeof(float));
	for (size_t i = 0; i < n; i++) {
		float v = h[i] >= sea ? log2f(1.0f + acc[i]) / RIVER_LOG_MAX : 0.0f;
		L[i] = v > 1.0f ? 1.0f : v;
		lake[i] = h[i] >= sea ? filled[i] - h[i] : 0.0f;
	}
	free(filled);

	/* Widen: max over a 5x5 window of (strength - falloff * distance). */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			size_t i = (size_t)y * W + x;
			if (h[i] < sea) { river[i] = 0.0f; continue; }
			float best = L[i];
			for (int dy = -2; dy <= 2; dy++) {
				int yy = y + dy;
				if (yy < 0 || yy >= H) continue;
				for (int dx = -2; dx <= 2; dx++) {
					if (!dx && !dy) continue;
					int xx = (x + dx + W) % W;
					float v = L[(size_t)yy * W + xx]
					        - RIVER_WIDEN * sqrtf((float)(dx * dx + dy * dy));
					if (v > best) best = v;
				}
			}
			river[i] = best;
		}
	free(L);
}

static unsigned char toByte(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (unsigned char)(v * 255.0f + 0.5f);
}

/* Builds and uploads the RGBA8 climate map (layout in TerrainGen.h). */
static GLuint bakeClimate(const float *h, const float *anom, float *dep, float sea,
                          int W, int H)
{
	size_t n = (size_t)W * H;
	float *moist = malloc(n * sizeof(float));
	float *river = malloc(n * sizeof(float));
	float *lake = malloc(n * sizeof(float));
	computeMoisture(h, anom, sea, W, H, moist);
	computeDrainage(h, moist, sea, W, H, river, lake);

	/* Alluvium: soften, then normalize by its 99th area percentile. */
	static Histo dh;
	blurField(dep, W, H, 2);
	histoBuild(dep, W, H, &dh);
	float dNorm = histoPctl(&dh, 0.99);
	if (!(dNorm > 1e-9f)) dNorm = 1e-9f;

	unsigned char *px = malloc(n * 4);
	for (size_t i = 0; i < n; i++) {
		px[i * 4 + 0] = toByte(moist[i]);
		px[i * 4 + 1] = toByte(river[i]);
		px[i * 4 + 2] = toByte(h[i] >= sea ? dep[i] / dNorm : 0.0f);
		px[i * 4 + 3] = toByte(lake[i] / LAKE_DEPTH_NORM);
	}
	free(moist); free(river); free(lake);

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Debug: TERRAIN_DUMP_CLIMATE names a file for the raw RGBA8 map. */
	const char *path = getenv("TERRAIN_DUMP_CLIMATE");
	if (path && *path) {
		FILE *f = fopen(path, "wb");
		if (f) { fwrite(px, 1, n * 4, f); fclose(f); }
	}
	free(px);
	return tex;
}

/* ---------------- Ocean map ---------------- */

/* Sea state for water.vert/.frag, from the same prevailing-wind bands that
   carry computeMoisture's rain, so the waves and the climate agree. */
#define OCEAN_FETCH_KM     400.0f   /* fetch at which the sea is ~63% developed */
#define PLANET_RADIUS_KM   (EARTH_RADIUS_M / 1000.0)  /* WorldConfig.h */
#define WIND_MAX_MS        20.0f    /* wind speed stored as R = 1 */
#define WIND_EDGE_DEG      5.0      /* half-width of the calm band between cells */
#define WIND_ANOMALY_AMT   0.6f     /* storm/calm regions from the climate anomaly */

static float smoothstepD(double e0, double e1, double x)
{
	double t = (x - e0) / (e1 - e0);
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	return (float)(t * t * (3.0 - 2.0 * t));
}

static double gaussBand(double x, double c, double w)
{
	return exp(-((x - c) / w) * ((x - c) / w));
}

/* Prevailing wind at |latitude| absLat: zonal sign -1 = from the east
   (trades, polar easterlies), +1 = from the west (westerlies), going
   through zero across the cell boundaries; equatorward / poleward drift
   (positive = poleward); and speed (m/s): calm doldrums and horse
   latitudes, steady trades, strong mid-latitude westerlies. */
static void prevailingWind(double absLat, float *zonal, float *pole, float *speed)
{
	float s1 = smoothstepD(WIND_TRADE_LAT - WIND_EDGE_DEG, WIND_TRADE_LAT + WIND_EDGE_DEG, absLat);
	float s2 = smoothstepD(WIND_POLAR_LAT - WIND_EDGE_DEG, WIND_POLAR_LAT + WIND_EDGE_DEG, absLat);
	*zonal = -1.0f + 2.0f * s1 - 2.0f * s2;
	*pole = -0.45f * (1.0f - s1) + 0.3f * (s1 - s2) - 0.3f * s2;
	*speed = (float)(3.0 + 4.5 * gaussBand(absLat, 17.0, 8.0)
	                     + 8.0 * gaussBand(absLat, 48.0, 10.0)
	                     + 3.0 * gaussBand(absLat, 70.0, 8.0));
}

/* RGBA8 ocean map (layout in TerrainGen.h). Fetch streams along each
   latitude row with the zonal wind, like the moisture: open water adds its
   length, land resets it, two laps converge the wrap. Lee coasts and the
   water behind islands stay calm; windward coasts face a developed sea. */
static GLuint bakeOcean(const float *h, const float *anom, float sea, int W, int H)
{
	size_t n = (size_t)W * H;
	float *fetch = malloc(n * sizeof(float));
	double texelKm = 2.0 * M_PI * PLANET_RADIUS_KM / (double)W;
	for (int y = 0; y < H; y++) {
		const float *row = h + (size_t)y * W;
		float *out = fetch + (size_t)y * W;
		double latDeg = fabs((((double)y + 0.5) / (double)H - 0.5) * 180.0);
		float zonal, pole, speed;
		prevailingWind(latDeg, &zonal, &pole, &speed);
		int dir = zonal >= 0.0f ? 1 : -1;   /* downwind along the row */
		float step = (float)(texelKm * rowCos[y]);
		float f = 0.0f;
		for (int lap = 0; lap < 2; lap++) {
			for (int k = 0; k < W; k++) {
				int x = dir > 0 ? k : W - 1 - k;
				f = row[x] < sea ? f + step : 0.0f;
				if (lap) out[x] = 1.0f - expf(-f / OCEAN_FETCH_KM);
			}
		}
	}
	/* Soften the row-by-row streaks into lee shadows. */
	blurField(fetch, W, H, 2);

	unsigned char *px = malloc(n * 4);
	for (int y = 0; y < H; y++) {
		double lat = (((double)y + 0.5) / (double)H - 0.5) * 180.0;
		float zonal, pole, speed;
		prevailingWind(fabs(lat), &zonal, &pole, &speed);
		float north = lat >= 0.0 ? pole : -pole;
		float len = sqrtf(zonal * zonal + north * north);
		float de = len > 1e-4f ? zonal / len : 1.0f;
		float dn = len > 1e-4f ? north / len : 0.0f;
		for (int x = 0; x < W; x++) {
			size_t i = (size_t)y * W + x;
			float u = speed * (1.0f + WIND_ANOMALY_AMT * anom[i]);
			px[i * 4 + 0] = toByte(u / WIND_MAX_MS);
			px[i * 4 + 1] = toByte(de * 0.5f + 0.5f);
			px[i * 4 + 2] = toByte(dn * 0.5f + 0.5f);
			px[i * 4 + 3] = toByte(fetch[i]);
		}
	}
	free(fetch);

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(px);
	return tex;
}

void setOceanUniforms(unsigned int shader, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, g_planetGen.oceanTex);
	glUniform1i(glGetUniformLocation(shader, "oceanMap"), unit);
	glUniform1f(glGetUniformLocation(shader, "windMax"), WIND_MAX_MS);
	glActiveTexture(GL_TEXTURE0);
}

/* ---------------- Surface style ---------------- */

/* Earth-like defaults. Colours are sRGB (as a colour picker shows them);
   setPlanetSurfaceUniforms converts them to linear albedo for the shader.
   Linear targets roughly: forest ~0.02-0.05, grass ~0.05-0.10,
   desert ~0.15-0.50, fresh snow ~0.85. */
PlanetStyle g_planetStyle = {
	{	/* cold:      tundra             boreal              taiga */
		{ { 0.47f, 0.44f, 0.38f }, { 0.30f, 0.33f, 0.24f }, { 0.16f, 0.22f, 0.14f } },
		/* temperate: steppe             grassland           forest */
		{ { 0.60f, 0.54f, 0.40f }, { 0.40f, 0.45f, 0.25f }, { 0.18f, 0.27f, 0.13f } },
		/* hot:       desert             savanna             rainforest */
		{ { 0.76f, 0.62f, 0.44f }, { 0.58f, 0.52f, 0.32f }, { 0.12f, 0.22f, 0.09f } } },
	{ 0.45f, 0.42f, 0.39f },   /* rock */
	{ 0.95f, 0.96f, 0.97f },   /* snow */
	{ 0.84f, 0.77f, 0.60f },   /* sand */
	{ 0.92f, 0.90f, 0.85f },   /* salt pan */
	{ 0.10f, 0.22f, 0.30f },   /* river / lake water */
	0.0f,    /* tempOffset */
	1.0f,    /* lapseRate */
	0.12f,   /* snowTemp */
	1.0f,    /* humidity */
	1.0f,    /* equatorLush */
	1.0f,    /* hadleyStrength */
	0.52f,   /* riverThreshold */
	0.55f,   /* rockSlope */
	0.025f,  /* landRelief */
	0.01f,   /* oceanRelief */
};

/* sRGB transfer function -> linear, componentwise over n floats. */
static void srgbToLinear(const float *in, float *out, int n)
{
	for (int i = 0; i < n; i++) {
		float c = in[i];
		out[i] = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}
}

/* Uploads an sRGB palette entry (count vec3s) as linear. */
static void uniformSrgb3(unsigned int shader, const char *name, const float *srgb, int count)
{
	float lin[9 * 3];
	srgbToLinear(srgb, lin, count * 3);
	glUniform3fv(glGetUniformLocation(shader, name), count, lin);
}

void setPlanetSurfaceUniforms(unsigned int shader, int unit)
{
	const PlanetStyle *ps = &g_planetStyle;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, g_planetGen.climateTex);
	glUniform1i(glGetUniformLocation(shader, "climateMap"), unit);
	uniformSrgb3(shader, "biomeColors", &ps->biome[0][0][0], 9);
	uniformSrgb3(shader, "rockColor", ps->rock, 1);
	uniformSrgb3(shader, "snowColor", ps->snow, 1);
	uniformSrgb3(shader, "sandColor", ps->sand, 1);
	uniformSrgb3(shader, "saltColor", ps->salt, 1);
	uniformSrgb3(shader, "inlandWaterColor", ps->water, 1);
	glUniform1f(glGetUniformLocation(shader, "tempOffset"), ps->tempOffset);
	glUniform1f(glGetUniformLocation(shader, "lapseRate"), ps->lapseRate);
	glUniform1f(glGetUniformLocation(shader, "snowTemp"), ps->snowTemp);
	glUniform1f(glGetUniformLocation(shader, "humidity"), ps->humidity);
	glUniform1f(glGetUniformLocation(shader, "equatorLush"), ps->equatorLush);
	glUniform1f(glGetUniformLocation(shader, "hadleyStrength"), ps->hadleyStrength);
	glUniform1f(glGetUniformLocation(shader, "riverThreshold"), ps->riverThreshold);
	glUniform1f(glGetUniformLocation(shader, "rockSlope"), ps->rockSlope);
	glActiveTexture(GL_TEXTURE0);
}

/* Debug: when TERRAIN_DUMP names a file, write the final normalized field
   there as raw float32 (W*H, row 0 = south pole) for offline inspection. */
static void debugDump(const float *h, size_t n)
{
	const char *path = getenv("TERRAIN_DUMP");
	if (!path || !*path) return;
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "[terrain] cannot write dump %s\n", path); return; }
	fwrite(h, sizeof(float), n, f);
	fclose(f);
}

/* ---------------- Tectonic plates ---------------- */

/* Plate centres (min-separated random points), continental flags (random
   plates until continentArea of the sphere, by Monte-Carlo cell area),
   rotation poles/speeds and passive-margin widths, uploaded to the shaping
   shader. Own PRNG so the erosion stream is unaffected. */
static unsigned int plRng;
static float plRand(void)
{
	plRng ^= plRng << 13; plRng ^= plRng >> 17; plRng ^= plRng << 5;
	return (float)(plRng >> 8) * (1.0f / 16777216.0f);
}
static void plRandDir(float v[3])
{
	float z = 2.0f * plRand() - 1.0f, a = 6.2831853f * plRand();
	float r = sqrtf(fmaxf(1.0f - z * z, 0.0f));
	v[0] = r * cosf(a); v[1] = r * sinf(a); v[2] = z;
}

static void uploadPlates(const TerrainBakeSettings *bs)
{
	float c[TERRAIN_PLATE_MAX][4], mo[TERRAIN_PLATE_MAX][4];
	int np = 0;
	int plateCount = bs->plateCount < 2 ? 2
	               : (bs->plateCount > TERRAIN_PLATE_MAX ? TERRAIN_PLATE_MAX : bs->plateCount);
	plRng = bs->seed * 747796405u + 2891336453u;
	if (!plRng) plRng = 1u;
	float minDot = cosf(PLATE_MIN_SEP);
	for (int tries = 0; np < plateCount && tries < 10000; tries++) {
		float v[3];
		plRandDir(v);
		int ok = 1;
		for (int k = 0; k < np && ok; k++)
			if (v[0] * c[k][0] + v[1] * c[k][1] + v[2] * c[k][2] > minDot) ok = 0;
		if (!ok) continue;
		c[np][0] = v[0]; c[np][1] = v[1]; c[np][2] = v[2]; c[np][3] = 0.0f;
		np++;
	}

	/* Cell areas (unwarped Voronoi) by uniform sampling. */
	int count[TERRAIN_PLATE_MAX] = { 0 };
	const int samples = 8192;
	for (int s = 0; s < samples; s++) {
		float v[3];
		plRandDir(v);
		int best = 0; float bd = -2.0f;
		for (int k = 0; k < np; k++) {
			float d = v[0] * c[k][0] + v[1] * c[k][1] + v[2] * c[k][2];
			if (d > bd) { bd = d; best = k; }
		}
		count[best]++;
	}
	/* Random order; mark continental until the area target (min 2). */
	int order[TERRAIN_PLATE_MAX];
	for (int k = 0; k < np; k++) order[k] = k;
	for (int k = np - 1; k > 0; k--) {
		int j = (int)(plRand() * (float)(k + 1)); if (j > k) j = k;
		int t = order[k]; order[k] = order[j]; order[j] = t;
	}
	float area = 0.0f;
	for (int k = 0, nc = 0; k < np; k++) {
		if (nc >= 2 && area >= bs->continentArea) break;
		c[order[k]][3] = 1.0f;
		area += (float)count[order[k]] / (float)samples;
		nc++;
	}

	for (int k = 0; k < np; k++) {
		float ax[3];
		plRandDir(ax);
		float speed = 0.5f + plRand();
		mo[k][0] = ax[0] * speed; mo[k][1] = ax[1] * speed; mo[k][2] = ax[2] * speed;
		mo[k][3] = plRand() * PLATE_MARGIN_MAX;
	}
	glUniform1i(glGetUniformLocation(bakeProgram, "plateCount"), np);
	glUniform4fv(glGetUniformLocation(bakeProgram, "plates"), np, &c[0][0]);
	glUniform4fv(glGetUniformLocation(bakeProgram, "plateMotion"), np, &mo[0][0]);
}

/* Shaping recipe uniforms (heightbake.frag keeps the #define names). */
static void uploadShaping(const TerrainBakeSettings *bs)
{
	glUniform1f(glGetUniformLocation(bakeProgram, "COAST_FREQ"), bs->coastFreq);
	glUniform1f(glGetUniformLocation(bakeProgram, "COAST_AMT"), bs->coastAmt);
	glUniform1f(glGetUniformLocation(bakeProgram, "PLAINS_H"), bs->plainsH);
	glUniform1f(glGetUniformLocation(bakeProgram, "HILL_AMT"), bs->hillAmt);
	glUniform1f(glGetUniformLocation(bakeProgram, "PLATEAU_H"), bs->plateauH);
	glUniform1f(glGetUniformLocation(bakeProgram, "COLLIDE_H"), bs->collideH);
	glUniform1f(glGetUniformLocation(bakeProgram, "ANDES_H"), bs->andesH);
	glUniform1f(glGetUniformLocation(bakeProgram, "OLD_RANGE_H"), bs->oldRangeH);
	glUniform1f(glGetUniformLocation(bakeProgram, "OCEAN_BASE"), bs->oceanBase);
	glUniform1f(glGetUniformLocation(bakeProgram, "RIDGE_H"), bs->ridgeH);
	glUniform1f(glGetUniformLocation(bakeProgram, "HOTSPOT_LO"), bs->hotspotLo);
	glUniform1f(glGetUniformLocation(bakeProgram, "FRAG_LO"), bs->fragLo);
	glUniform1f(glGetUniformLocation(bakeProgram, "DETAIL_AMT"), bs->detailAmt);
}

/* ---------------- Full bake ---------------- */

unsigned int generatePlanetHeightmap(const TerrainBakeSettings *bs, PlanetGenParams *out,
                                     void (*step)(void))
{
	const unsigned int seed = bs->seed;
	const int W = HEIGHTMAP_W, H = HEIGHTMAP_H;
	const size_t n = (size_t)W * H;
	static Histo hf;   /* 32 KB: keep it off the stack */

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
	uploadPlates(bs);
	uploadShaping(bs);
	glBindVertexArray(bakeQuadVAO);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	clock_t t1 = clock();

	/* 2. Readback to a CPU float buffer (r channel holds the shaped h). */
	float *rgba = malloc(n * 4 * sizeof(float));
	float *h = malloc(n * sizeof(float));
	if (!rgba || !h) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	float *anom = malloc(n * sizeof(float));
	float *dep = calloc(n, sizeof(float));
	if (!anom || !dep) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	for (size_t i = 0; i < n; i++) { h[i] = rgba[i * 4]; anom[i] = rgba[i * 4 + 1]; }
	free(rgba);
	clampFinite(h, n);
	clampFinite(anom, n);

	/* Pre-normalize so the erosion constants are in fixed [0,1] units. The
	   shaper's sea level is raw 0; track where it lands. */
	histoBuild(h, W, H, &hf);
	float preLo = histoPctl(&hf, PRE_LO_PCT), preHi = histoPctl(&hf, PRE_HI_PCT);
	stretch01(h, n, preLo, preHi);
	float seaLevel0 = (0.0f - preLo) / fmaxf(preHi - preLo, 1e-6f);
	clock_t t2 = clock();

	/* 3. Thermal erosion (weathering), then 4. hydraulic droplet erosion. */
	thermalErode(h, W, H, bs->thermalPasses, bs->thermalTalus, step);
	clock_t t3 = clock();

	/* Droplets spawn only on land; reaching the sea is a river mouth. */
	hydraulicErode(h, dep, W, H, seaLevel0, bs, step);
	clock_t t4 = clock();

	/* 5. Percentile-stretch the relief to [0,1], carrying the sea level
	   (raw 0, set by the plate crust) through it, + ocean depth (95th area
	   percentile of depth below sea level, feeds the water.frag tint). */
	histoBuild(h, W, H, &hf);
	float lo = histoPctl(&hf, NORMALIZE_LO_PCT);
	float hi = histoPctl(&hf, NORMALIZE_HI_PCT);
	float range = hi - lo;
	if (range < 1e-6f) range = 1.0f;
	float seaLevel = (seaLevel0 - lo) / range;
	seaLevel = seaLevel < 0.0f ? 0.0f : (seaLevel > 1.0f ? 1.0f : seaLevel);
	stretch01(h, n, lo, hi);

	/* Area-weighted depth histogram over the ocean texels: depths lie in
	   (0, seaLevel]. */
	static double dhist[PCTL_BINS];
	memset(dhist, 0, sizeof dhist);
	double oceanArea = 0.0;
	float oceanDepth = 0.15f;
	if (seaLevel > 1e-6f) {
		double dscale = (double)(PCTL_BINS - 1) / (double)seaLevel;
		for (int y = 0; y < H; y++) {
			const float *row = h + (size_t)y * W;
			double w = rowCos[y];
			for (int x = 0; x < W; x++) {
				if (row[x] < seaLevel) {
					oceanArea += w;
					int b = (int)((double)(seaLevel - row[x]) * dscale);
					dhist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)] += w;
				}
			}
		}
		if (oceanArea > 0.0) {
			double target = OCEAN_DEPTH_PCT * oceanArea;
			double acc = 0.0;
			int bin = PCTL_BINS - 1;
			for (int b = 0; b < PCTL_BINS; b++) {
				acc += dhist[b];
				if (acc > target) { bin = b; break; }
			}
			oceanDepth = ((float)bin + 0.5f) / (float)dscale;
		}
	}
	if (!(oceanDepth > 1e-3f)) oceanDepth = 1e-3f;

	debugDump(h, n);
	clock_t t5 = clock();

	/* 7. Climate map: moisture, rivers, alluvium, lakes. */
	if (g_planetGen.climateTex) glDeleteTextures(1, &g_planetGen.climateTex);
	out->climateTex = bakeClimate(h, anom, dep, seaLevel, W, H);
	if (g_planetGen.oceanTex) glDeleteTextures(1, &g_planetGen.oceanTex);
	out->oceanTex = bakeOcean(h, anom, seaLevel, W, H);
	free(anom);
	free(dep);
	out->seaLevel = seaLevel;
	out->oceanDepth = oceanDepth;
	g_planetGen = *out;
	clock_t t6 = clock();

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
	fprintf(stderr, "[terrain] bake seed %u: shape %.2f s, readback %.2f s, thermal %.2f s, hydro %.2f s, sea %.2f s, climate %.2f s -> seaLevel %.4f, oceanDepth %.4f\n",
	        seed,
	        (double)(t1 - t0) / s, (double)(t2 - t1) / s, (double)(t3 - t2) / s,
	        (double)(t4 - t3) / s, (double)(t5 - t4) / s, (double)(t6 - t5) / s,
	        seaLevel, oceanDepth);

	return heightTex;
}
