#include "Main.h"
#include "TerrainGen.h"
#include "WorldConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include <process.h>

/* The project builds without /O2; the bake's CPU erosion needs real
   optimization to stay inside the 1-3 s startup budget. Scoped to this
   translation unit so no other module's codegen changes. */
#pragma optimize("gtp", on)

/* Sea level + ocean depth used by quad.frag/water.frag (see TerrainGen.h). */
PlanetGenParams g_planetGen = { 0.5f, 0.22f, 0 };

/* The baked maps are cube maps (TERRAIN_MAP_SIZE texels per face edge, GL
   face order +X -X +Y -Y +Z -Z). CPU fields hold the six faces back to
   back, texel (i, j) of face f at (f*N + j)*N + i, i along the face's s
   axis, j along t (GL rows). */
#define FACE_N      TERRAIN_MAP_SIZE
#define FACE_TEXELS ((size_t)FACE_N * FACE_N)
#define CUBE_TEXELS (6 * FACE_TEXELS)

/* Reference grid: the erosion and climate constants below were tuned on a
   2048x1024 equirect map, in units of its equator texel (PI/1024 rad).
   They keep those units; the cube code converts with its own texel sizes,
   so the physics stays the same at any TERRAIN_MAP_SIZE. Moisture and wave
   fetch stream along latitude rows, so they still run on a grid of this
   size and are resampled onto the cube (smooth fields: no pinch shows). */
#define EQ_W 2048
#define EQ_H 1024
#define REF_TEXEL_RAD (M_PI / 1024.0)

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

static float rowCos[EQ_H];

/* cos(latitude) of every texel row's centre: the row's area weight. */
static void initRowCos(int H)
{
	for (int y = 0; y < H; y++) {
		double lat = (((double)y + 0.5) / (double)H - 0.5) * M_PI;
		float c = (float)cos(lat);
		rowCos[y] = c < MIN_COS_LAT ? MIN_COS_LAT : c;
	}
}

/* ---------------- Cube map geometry ---------------- */

/* GL's cube faces (the spec's major-axis table): a face point with face
   coordinates sc, tc in [-1, 1] lies along M + sc*S + tc*T. */
static const double faceM[6][3] = {
	{ 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, { 0, 0, -1} };
static const double faceS[6][3] = {
	{ 0, 0, -1}, { 0, 0, 1}, {1,  0, 0}, {1,  0, 0}, {1, 0,  0}, {-1, 0,  0} };
static const double faceT[6][3] = {
	{ 0, -1, 0}, { 0, -1, 0}, {0,  0, 1}, {0,  0, -1}, {0, -1, 0}, { 0, -1, 0} };

static inline double dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Direction (not normalized) of face point (sc, tc); also valid past the
   face edge, on the face's plane. */
static inline void faceDir(int f, double sc, double tc, double d[3])
{
	for (int k = 0; k < 3; k++)
		d[k] = faceM[f][k] + sc * faceS[f][k] + tc * faceT[f][k];
}

/* The face a direction lands on and its face coordinates there. */
static inline int dirFace(const double d[3], double *sc, double *tc)
{
	double ax = fabs(d[0]), ay = fabs(d[1]), az = fabs(d[2]);
	int f = (ax >= ay && ax >= az) ? (d[0] >= 0.0 ? 0 : 1)
	      : (ay >= az ? (d[1] >= 0.0 ? 2 : 3) : (d[2] >= 0.0 ? 4 : 5));
	double ma = dot3(d, faceM[f]);
	*sc = dot3(d, faceS[f]) / ma;
	*tc = dot3(d, faceT[f]) / ma;
	return f;
}

/* Face coordinate of texel centre i on an n-texel face edge, and back. */
static inline double texelToFace(double i, int n) { return (2.0 * i + 1.0) / n - 1.0; }
static inline double faceToTexel(double s, int n) { return (s + 1.0) * 0.5 * n - 0.5; }

/* Index of texel (i, j) of face f in an n-per-edge cube field. i and j may
   lie off the face (a few texels): the texel is then found across the edge
   by projecting its centre onto the neighbouring face. One texel past an
   edge this gives exactly the adjacent texel of the next face (the texel
   grids meet edge to edge), the same neighbour seamless filtering uses. */
static size_t cubeIndex(int n, int f, int i, int j)
{
	if ((unsigned)i < (unsigned)n && (unsigned)j < (unsigned)n)
		return ((size_t)f * n + j) * n + i;
	double d[3], sc, tc;
	faceDir(f, texelToFace(i, n), texelToFace(j, n), d);
	int g = dirFace(d, &sc, &tc);
	int ii = (int)floor((sc + 1.0) * 0.5 * n), jj = (int)floor((tc + 1.0) * 0.5 * n);
	ii = ii < 0 ? 0 : (ii > n - 1 ? n - 1 : ii);
	jj = jj < 0 ? 0 : (jj > n - 1 ? n - 1 : jj);
	return ((size_t)g * n + jj) * n + ii;
}

/* Unit direction of texel (i, j) of face f (FACE_N per edge). */
static inline void texelDir(int f, int i, int j, double d[3])
{
	faceDir(f, texelToFace(i, FACE_N), texelToFace(j, FACE_N), d);
	double inv = 1.0 / sqrt(dot3(d, d));
	d[0] *= inv; d[1] *= inv; d[2] *= inv;
}

/* Per-texel metric, the same on every face (index j*N + i): area = solid
   angle and spS / spT = centre spacing to the next texel along s / t, in
   reference-texel units (REF_TEXEL_RAD, its square for the area). The
   plain projection's texels shrink toward the face corners, to ~1/3 of
   the centre's area. */
static float *texArea, *texSpS, *texSpT;
static float texAreaMax;

static void initCubeMetric(void)
{
	if (texArea) return;
	texArea = malloc(FACE_TEXELS * sizeof(float));
	texSpS = malloc(FACE_TEXELS * sizeof(float));
	texSpT = malloc(FACE_TEXELS * sizeof(float));
	if (!texArea || !texSpS || !texSpT) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	double t = 2.0 / FACE_N / REF_TEXEL_RAD;   /* face-centre texel, ref units */
	texAreaMax = 0.0f;
	for (int j = 0; j < FACE_N; j++)
		for (int i = 0; i < FACE_N; i++) {
			double sc = texelToFace(i, FACE_N), tc = texelToFace(j, FACE_N);
			double r2 = 1.0 + sc * sc + tc * tc;
			size_t k = (size_t)j * FACE_N + i;
			texArea[k] = (float)(t * t / (r2 * sqrt(r2)));
			texSpS[k] = (float)(t * sqrt(1.0 + tc * tc) / r2);
			texSpT[k] = (float)(t * sqrt(1.0 + sc * sc) / r2);
			if (texArea[k] > texAreaMax) texAreaMax = texArea[k];
		}
}

/* Bilinear sample of a cube field along direction d (clamped at the face
   edges: for the smooth climate fields only). */
static float sampleCube(const float *field, const double d[3])
{
	double sc, tc;
	int f = dirFace(d, &sc, &tc);
	double x = faceToTexel(sc, FACE_N), y = faceToTexel(tc, FACE_N);
	x = x < 0.0 ? 0.0 : (x > FACE_N - 1 ? FACE_N - 1 : x);
	y = y < 0.0 ? 0.0 : (y > FACE_N - 1 ? FACE_N - 1 : y);
	int x0 = (int)x, y0 = (int)y;
	if (x0 > FACE_N - 2) x0 = FACE_N - 2;
	if (y0 > FACE_N - 2) y0 = FACE_N - 2;
	float fx = (float)(x - x0), fy = (float)(y - y0);
	const float *r0 = field + ((size_t)f * FACE_N + y0) * FACE_N + x0;
	const float *r1 = r0 + FACE_N;
	return (r0[0] * (1.0f - fx) + r0[1] * fx) * (1.0f - fy) + (r1[0] * (1.0f - fx) + r1[1] * fx) * fy;
}

/* Bilinear sample of an EQ_W x EQ_H equirect field along unit direction d
   (longitude wraps, latitude clamps). */
static float sampleEquirect(const float *field, const double d[3])
{
	double lon = atan2(d[1], d[0]);
	double lat = asin(d[2] < -1.0 ? -1.0 : (d[2] > 1.0 ? 1.0 : d[2]));
	double x = (lon / M_PI + 1.0) * 0.5 * EQ_W - 0.5;
	double y = (lat / M_PI + 0.5) * EQ_H - 0.5;
	y = y < 0.0 ? 0.0 : (y > EQ_H - 1 ? EQ_H - 1 : y);
	int x0 = (int)floor(x), y0 = (int)y;
	if (y0 > EQ_H - 2) y0 = EQ_H - 2;
	float fx = (float)(x - x0), fy = (float)(y - y0);
	int x1 = ((x0 + 1) % EQ_W + EQ_W) % EQ_W;
	x0 = (x0 % EQ_W + EQ_W) % EQ_W;
	const float *r0 = field + (size_t)y0 * EQ_W, *r1 = r0 + EQ_W;
	return (r0[x0] * (1.0f - fx) + r0[x1] * fx) * (1.0f - fy) + (r1[x0] * (1.0f - fx) + r1[x1] * fx) * fy;
}

/* Resamples a cube field onto a new EQ_W x EQ_H equirect field. */
static float *cubeToEquirect(const float *field)
{
	float *eq = malloc((size_t)EQ_W * EQ_H * sizeof(float));
	for (int y = 0; y < EQ_H; y++) {
		double lat = (((double)y + 0.5) / EQ_H - 0.5) * M_PI;
		for (int x = 0; x < EQ_W; x++) {
			double lon = (((double)x + 0.5) / EQ_W * 2.0 - 1.0) * M_PI;
			double d[3] = { cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat) };
			eq[(size_t)y * EQ_W + x] = sampleCube(field, d);
		}
	}
	return eq;
}

/* Resamples an equirect field onto a new cube field. */
static float *equirectToCube(const float *eq)
{
	float *field = malloc(CUBE_TEXELS * sizeof(float));
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < FACE_N; j++)
			for (int i = 0; i < FACE_N; i++) {
				double d[3];
				texelDir(f, i, j, d);
				field[((size_t)f * FACE_N + j) * FACE_N + i] = sampleEquirect(eq, d);
			}
	return field;
}

/* Area-weighted percentiles via histogram (no sort): each texel counts with
   its area, so small texels (equirect polar rows, cube face corners) don't
   skew the stats. With 4096 bins over a smooth distribution the error is
   well below one texel's height step, and it keeps multi-million-value
   percentiles cheap (MSVC qsort measured ~1.7 s at 2M). One build answers
   several queries. */
#define PCTL_BINS 4096
typedef struct {
	float lo, hi;
	double total;
	double binToVal;
	double hist[PCTL_BINS];
} Histo;

/* Area weight of value k: w[(k / div) % period]. Equirect: div = width,
   w = rowCos; cube: div = 1, w = texArea (one face's texels). */
static void histoBuild(const float *buf, size_t n, const float *w, size_t div, size_t period,
                       Histo *Hs)
{
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
	for (size_t base = 0, blk = 0; base < n; base += div, blk++) {
		double wt = w[blk % period];
		size_t end = base + div < n ? base + div : n;
		for (size_t i = base; i < end; i++) {
			int b = (int)((double)(buf[i] - lo) * scale);
			Hs->hist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)] += wt;
		}
		Hs->total += wt * (double)(end - base);
	}
}

static void histoBuildCube(const float *buf, Histo *Hs)
{
	histoBuild(buf, CUBE_TEXELS, texArea, 1, FACE_TEXELS, Hs);
}

static void histoBuildEq(const float *buf, Histo *Hs)
{
	histoBuild(buf, (size_t)EQ_W * EQ_H, rowCos, EQ_W, EQ_H, Hs);
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
   the sweep backwards to cancel the bias. talus is per reference texel of
   distance; each pair's is scaled by its actual spacing, so the stable
   slope is the same everywhere on the faces. Edge texels pair with the
   neighbouring face's. */
static void thermalErode(float *h, int passes, float talus, void (*step)(void))
{
	const int N = FACE_N;
	int stepEvery = passes / TERRAIN_THERMAL_STEPS;
	if (stepEvery < 1) stepEvery = 1;
	int stepsTaken = 0;
	for (int p = 0; p < passes; p++) {
		if (step && stepsTaken < TERRAIN_THERMAL_STEPS && p % stepEvery == 0) {
			step();
			stepsTaken++;
		}
		int rev = p & 1;
		for (int ff = 0; ff < 6; ff++) {
			int f = rev ? 5 - ff : ff;
			for (int jj = 0; jj < N; jj++) {
				int j = rev ? N - 1 - jj : jj;
				size_t row = ((size_t)f * N + j) * N;
				const float *spS = texSpS + (size_t)j * N, *spT = texSpT + (size_t)j * N;
				int edgeRow = j == 0 || j == N - 1;
				for (int ii = 0; ii < N; ii++) {
					int i = rev ? N - 1 - ii : ii;
					size_t c = row + i;
					float hc = h[c];
					float tS = talus * spS[i], tT = talus * spT[i];
					#define TPAIR(n, t) { \
							size_t nj = (n); \
							float hn = h[nj]; \
							if (hn < hc) { \
								float excess = hc - hn - (t); \
								if (excess > 0.0f) { \
									float mv = 0.5f * excess; \
									hc -= mv; h[nj] += mv; \
								} \
							} }
					if (edgeRow || i == 0 || i == N - 1) {
						TPAIR(cubeIndex(N, f, i - 1, j), tS);
						TPAIR(cubeIndex(N, f, i + 1, j), tS);
						TPAIR(cubeIndex(N, f, i, j - 1), tT);
						TPAIR(cubeIndex(N, f, i, j + 1), tT);
					} else {
						TPAIR(c - 1, tS);
						TPAIR(c + 1, tS);
						TPAIR(c - N, tT);
						TPAIR(c + N, tT);
					}
					#undef TPAIR
					h[c] = hc;
				}
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

/* Droplet positions are texel-centre coordinates on one face: texel i's
   centre is x = i, and the face spans [-0.5, N - 0.5]. Sampling and
   deposits use the position clamped to [0, N - 1 - CLAMP_EPS] (the half
   texel strip along each edge reads the edge cell). */
#define CLAMP_EPS 1e-3f
static inline float clampFace(float x)
{
	return x < 0.0f ? 0.0f : (x > FACE_N - 1 - CLAMP_EPS ? FACE_N - 1 - CLAMP_EPS : x);
}

/* Bilinear height + gradient (per texel) at a clamped position on one face
   (face points at the face's first texel). */
static inline float sampleHG(const float *face, float x, float y, float *gx, float *gy)
{
	int x0 = (int)x, y0 = (int)y;
	float fx = x - (float)x0, fy = y - (float)y0;
	const float *r0 = face + (size_t)y0 * FACE_N + x0;
	const float *r1 = r0 + FACE_N;
	float h00 = r0[0], h10 = r0[1], h01 = r1[0], h11 = r1[1];
	*gx = (h10 - h00) * (1.0f - fy) + (h11 - h01) * fy;
	*gy = (h01 - h00) * (1.0f - fx) + (h11 - h10) * fx;
	return h00 * (1.0f - fx) * (1.0f - fy) + h10 * fx * (1.0f - fy)
	     + h01 * (1.0f - fx) * fy + h11 * fx * fy;
}

/* Spread amt over the brush centred on texel (cx, cy) of face f: positive
   deposits (never raising a texel above ceil), negative erodes (never
   below zero). Near an edge the brush reaches across into the next face.
   Returns the amount actually moved (signed like amt). */
static inline float applyBrush(float *h, int f, int cx, int cy, float amt, float ceil)
{
	const int R = DROPLET_BRUSH_RADIUS;
	int inside = cx >= R && cy >= R && cx < FACE_N - R && cy < FACE_N - R;
	size_t c = ((size_t)f * FACE_N + cy) * FACE_N + cx;
	float moved = 0.0f;
	for (int k = 0; k < brushN; k++) {
		size_t i = inside ? c + (ptrdiff_t)brushDY[k] * FACE_N + brushDX[k]
		                  : cubeIndex(FACE_N, f, cx + brushDX[k], cy + brushDY[k]);
		float *t = &h[i];
		float d = amt * brushW[k];
		if (d < 0.0f && -d > *t) d = -*t;
		if (d > 0.0f && *t + d > ceil) d = fmaxf(ceil - *t, 0.0f);
		*t += d;
		moved += d;
	}
	return moved;
}

/* A droplet at (x, y) on face f stepped to (nx, ny) off the face: moves it
   onto the face its new position lands on, and turns its heading (dx, dy:
   unit, in that face's physical s/t frame) to match. */
static int crossFace(int f, float x, float y, float *nx, float *ny, float *dx, float *dy)
{
	double d[3], p[3], sc, tc;
	faceDir(f, texelToFace(*nx, FACE_N), texelToFace(*ny, FACE_N), d);
	faceDir(f, texelToFace(x, FACE_N), texelToFace(y, FACE_N), p);
	int g = dirFace(d, &sc, &tc);
	/* The previous position on g's plane gives the heading there. */
	double ma = dot3(p, faceM[g]);
	double psc = dot3(p, faceS[g]) / ma, ptc = dot3(p, faceT[g]) / ma;
	*nx = (float)faceToTexel(sc, FACE_N);
	*ny = (float)faceToTexel(tc, FACE_N);
	size_t k = (size_t)(int)clampFace(*ny) * FACE_N + (int)clampFace(*nx);
	float ex = (float)(sc - psc) * texSpS[k], ey = (float)(tc - ptc) * texSpT[k];
	float len = sqrtf(ex * ex + ey * ey);
	if (len > 1e-12f) { *dx = ex / len; *dy = ey / len; }
	return g;
}

/* Hydraulic erosion, droplet method (Mei et al. 2007 / Lague): droplets
   spawn on land only (above the pre-erosion sea level) with an area-uniform
   density (rejection by texel area), move downhill with inertia, erode
   with a radial brush up to their transport capacity (never more than the
   height they just dropped, so they can't dig pits) and deposit the
   surplus. Uphill steps fill the pit behind them. A droplet that reaches
   the sea drops its load there (river deltas) and dies, so the seafloor is
   never carved. Headings and gradients are taken in physical units (the
   face texels' spacing varies), and droplets carry on across face edges.
   The droplet count, path length and per-distance rates are the reference
   grid's (one step = one face-centre texel; lifetime, evaporation, erosion
   and deposition rates convert, and capacity follows the slope rather than
   the drop per step), so the erosion is the same at any map size; the
   brush stays 2 texels, so channels get as sharp as the map. Every on-land
   deposit is also summed into dep (alluvial plains for the climate map). */
static void hydraulicErode(float *h, float *dep, float seaLevel0,
                           const TerrainBakeSettings *bs, void (*step)(void))
{
	const int N = FACE_N;
	long drops = (long)((double)bs->dropletsPerPx * EQ_W * EQ_H);
	const float stepLen = (float)(2.0 / N / REF_TEXEL_RAD);   /* ref texels per step */
	const int maxSteps = (int)(DROPLET_MAX_STEPS / stepLen + 0.5f);
	const float evapKeep = powf(1.0f - bs->dropletEvap, stepLen);
	const float erodeRate = 1.0f - powf(1.0f - bs->dropletErosion, stepLen);
	const float depositRate = 1.0f - powf(1.0f - bs->dropletDeposit, stepLen);
	const float invStep = 1.0f / stepLen;
	const float inertia = bs->dropletInertia;
	long stepEvery = drops / TERRAIN_HYDRO_STEPS;
	if (stepEvery < 1) stepEvery = 1;
	int stepsTaken = 0;
	for (long d = 0; d < drops; d++) {
		if (step && stepsTaken < TERRAIN_HYDRO_STEPS && d % stepEvery == 0) {
			step();
			stepsTaken++;
		}
		/* Spawn: random land texel, accepted with probability by area. */
		float x = 0.0f, y = 0.0f;
		int f = 0, found = 0;
		for (int tries = 0; tries < 32; tries++) {
			f = (int)(xsRand() * 6.0f);
			if (f > 5) f = 5;
			x = xsRand() * (float)(N - 1);
			y = xsRand() * (float)(N - 1);
			size_t k = (size_t)(int)y * N + (int)x;
			if (xsRand() * texAreaMax > texArea[k]) continue;
			if (h[(size_t)f * FACE_TEXELS + k] > seaLevel0) { found = 1; break; }
		}
		if (!found) continue;

		float dx = 0.0f, dy = 0.0f;
		float speed = DROPLET_START_SPEED;
		float water = DROPLET_START_WATER;
		float sediment = 0.0f;
		float gx, gy;
		float hCur = sampleHG(h + (size_t)f * FACE_TEXELS, x, y, &gx, &gy);

		for (int s = 0; s < maxSteps; s++) {
			float xs = clampFace(x), ys = clampFace(y);
			int cx = (int)xs, cy = (int)ys;
			size_t k = (size_t)cy * N + cx;
			float spS = texSpS[k], spT = texSpT[k];

			/* Gradient per reference texel of distance. */
			float ndx = dx * inertia - gx / spS * (1.0f - inertia);
			float ndy = dy * inertia - gy / spT * (1.0f - inertia);
			float len = sqrtf(ndx * ndx + ndy * ndy);
			if (len < 1e-9f) break;           /* stalled/flat: kill, never divide */
			dx = ndx / len; dy = ndy / len;

			float nx = x + dx * stepLen / spS;
			float ny = y + dy * stepLen / spT;
			int nf = f;
			if (nx < -0.5f || ny < -0.5f || nx > N - 0.5f || ny > N - 0.5f)
				nf = crossFace(f, x, y, &nx, &ny, &dx, &dy);

			float ngx, ngy;
			float hNew = sampleHG(h + (size_t)nf * FACE_TEXELS, clampFace(nx), clampFace(ny), &ngx, &ngy);
			float dh = hNew - hCur;

			if (hNew < seaLevel0) {
				/* River mouth: drop the load as shallows and stop. Capped
				   just under sea level so deltas never surface as one-texel
				   specks along the coast; the rest is carried out to sea. */
				if (sediment > 0.0f)
					applyBrush(h, nf, (int)clampFace(nx), (int)clampFace(ny), sediment,
					           seaLevel0 - DELTA_DEPTH);
				break;
			}

			float capacity = fmaxf(-dh * invStep, DROPLET_MIN_SLOPE) * speed * water * bs->dropletCapacity;
			if (sediment > capacity || dh > 0.0f) {
				float amt = (dh > 0.0f) ? fminf(dh, sediment)
				                        : (sediment - capacity) * depositRate;
				if (amt > 0.0f) {
					/* Bilinear deposit at the droplet's own position. */
					float fx = xs - (float)cx, fy = ys - (float)cy;
					float *r0 = h + (size_t)f * FACE_TEXELS + k;
					float *r1 = r0 + N;
					r0[0] += amt * (1.0f - fx) * (1.0f - fy);
					r0[1] += amt * fx * (1.0f - fy);
					r1[0] += amt * (1.0f - fx) * fy;
					r1[1] += amt * fx * fy;
					dep[(size_t)f * FACE_TEXELS + k] += amt;
					sediment -= amt;
				}
			} else {
				float amt = fminf((capacity - sediment) * erodeRate, -dh);
				if (amt > 0.0f)
					sediment -= applyBrush(h, f, cx, cy, -amt, FLT_MAX);
			}

			speed = sqrtf(fmaxf(speed * speed - dh * DROPLET_GRAVITY, 0.0f));
			water *= evapKeep;
			/* Carry the new sample forward (the brush may have nudged it
			   slightly; re-sampling costs a third of the loop for no
			   visible gain). */
			f = nf; x = nx; y = ny;
			hCur = hNew; gx = ngx; gy = ngy;
		}
	}
}

/* ---------------- GPU shaping (cached across re-bakes) ---------------- */

static unsigned int bakeProgram, bakeQuadVAO, workFBO, workTex;

static void ensureBakeResources(void)
{
	if (bakeProgram) return;

	createShader(&bakeProgram, "heightbake.vert", "heightbake.frag");

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

	/* Working RGBA16F target, read back as GL_RG / GL_FLOAT (the two
	   channels the bake uses): the six faces stacked bottom to top, so a
	   readback row is face * N + j, the CPU field layout. */
	glGenTextures(1, &workTex);
	glBindTexture(GL_TEXTURE_2D, workTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, FACE_N, 6 * FACE_N, 0,
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

	initRowCos(EQ_H);
	initCubeMetric();
	initBrush();
}

/* ---------------- Climate map ---------------- */

/* Separable box blur of a cube field, radius r texels, two passes ~ a
   soft tent. Each face is blurred from a copy padded with its neighbours'
   texels, so the result is continuous across the edges. */
static void blurCube(float *field, int r)
{
	const int N = FACE_N, P = N + 2 * r;
	float *pad = malloc((size_t)P * P * sizeof(float));
	float *tmp = malloc((size_t)P * P * sizeof(float));
	float *out = malloc(CUBE_TEXELS * sizeof(float));
	double inv = 1.0 / (double)(2 * r + 1);
	for (int pass = 0; pass < 2; pass++) {
		for (int f = 0; f < 6; f++) {
			for (int y = -r; y < N + r; y++)
				for (int x = -r; x < N + r; x++)
					pad[(size_t)(y + r) * P + x + r] = field[cubeIndex(N, f, x, y)];
			/* Rows: running window sums for padded columns [r, P - r). */
			for (int y = 0; y < P; y++) {
				const float *row = pad + (size_t)y * P;
				double acc = 0.0;
				for (int k = 0; k < 2 * r + 1; k++) acc += row[k];
				for (int x = r; x < P - r; x++) {
					tmp[(size_t)y * P + x] = (float)(acc * inv);
					if (x + r + 1 < P) acc += row[x + r + 1] - row[x - r];
				}
			}
			/* Columns, into the face's interior. */
			float *dst = out + (size_t)f * FACE_TEXELS;
			for (int x = 0; x < N; x++) {
				double acc = 0.0;
				for (int k = 0; k < 2 * r + 1; k++) acc += tmp[(size_t)k * P + x + r];
				for (int y = 0; y < N; y++) {
					dst[(size_t)y * N + x] = (float)(acc * inv);
					if (y + 2 * r + 1 < P)
						acc += tmp[(size_t)(y + 2 * r + 1) * P + x + r] - tmp[(size_t)y * P + x + r];
				}
			}
		}
		memcpy(field, out, CUBE_TEXELS * sizeof(float));
	}
	free(out);
	free(tmp);
	free(pad);
}

/* Separable box blur of an equirect field, x radius scaled by 1/cos(lat)
   so the footprint is round on the sphere; x wraps, y clamps. Two passes ~
   a soft tent. */
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
   they stay tunable per planet. Runs on the EQ_W x EQ_H equirect grid. */
static void computeMoisture(const float *h, const float *anom, float sea, float *m)
{
	const int W = EQ_W, H = EQ_H;
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
	histoBuildEq(landVals, &mh);
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
   style of the planet being baked; re-bake (H) after changing it to update
   rivers. */
static float hadleyFactorC(const PlanetStyle *ps, double absLatDeg)
{
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
   tree gives each texel's discharge (in reference-texel areas, so the
   river constants hold at any map size). River strength = log2 discharge,
   widened by RIVER_WIDEN per texel so big rivers survive the mip chain.
   Runs on the cube faces; edge texels' neighbours are across the edge. */
static void computeDrainage(const PlanetStyle *ps, const float *h, const float *moist, float sea,
                            float *river, float *lake)
{
	const int N = FACE_N;
	const size_t n = CUBE_TEXELS;
	float *filled = malloc(n * sizeof(float));
	int *recv = malloc(n * sizeof(int));
	int *order = malloc(n * sizeof(int));
	unsigned char *done = calloc(n, 1);
	pq = malloc(n * sizeof(PQNode));
	pqN = 0;
	static const int NDX[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
	static const int NDY[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
	/* Neighbour k of texel c (face f, texel i, j). */
	#define NBR(c, f, i, j, k) \
		(((i) > 0 && (i) < N - 1 && (j) > 0 && (j) < N - 1) \
		 ? (int)((c) + NDY[k] * N + NDX[k]) \
		 : (int)cubeIndex(N, (f), (i) + NDX[k], (j) + NDY[k]))

	for (size_t i = 0; i < n; i++) {
		recv[i] = -1;
		if (h[i] < sea) { done[i] = 1; filled[i] = h[i]; }
	}
	/* Seeds: ocean texels touching land. */
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++) {
				int c = (int)(((size_t)f * N + j) * N + i);
				if (h[c] >= sea) continue;
				for (int k = 0; k < 8; k++)
					if (h[NBR(c, f, i, j, k)] >= sea) { pqPush(h[c], c); break; }
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
		int f = c.i / (int)FACE_TEXELS, rem = c.i % (int)FACE_TEXELS;
		int i = rem % N, j = rem / N;
		for (int k = 0; k < 8; k++) {
			int q = NBR(c.i, f, i, j, k);
			if (done[q]) continue;
			done[q] = 1;
			filled[q] = fmaxf(h[q], c.h);
			recv[q] = c.i;
			pqPush(filled[q], q);
		}
	}
	free(pq); pq = NULL;
	free(done);

	/* Rain-weighted, area-weighted discharge, upstream first. */
	float *acc = river;   /* reuse the output buffer for the accumulation */
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++) {
				size_t c = ((size_t)f * N + j) * N + i;
				if (h[c] < sea) { acc[c] = 0.0f; continue; }
				double d[3];
				texelDir(f, i, j, d);
				float area = texArea[(size_t)j * N + i];
				double latDeg = asin(fmin(fabs(d[2]), 1.0)) * (180.0 / M_PI);
				float r = 2.0f * moist[c] * hadleyFactorC(ps, latDeg) * area;
				acc[c] = RIVER_RAIN_FLOOR * area + (r > 0.0f ? r : 0.0f);
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
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++) {
				size_t c = ((size_t)f * N + j) * N + i;
				if (h[c] < sea) { river[c] = 0.0f; continue; }
				int inside = i >= 2 && j >= 2 && i < N - 2 && j < N - 2;
				float best = L[c];
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++) {
						if (!dx && !dy) continue;
						size_t q = inside ? c + (ptrdiff_t)dy * N + dx
						                  : cubeIndex(N, f, i + dx, j + dy);
						float v = L[q] - RIVER_WIDEN * sqrtf((float)(dx * dx + dy * dy));
						if (v > best) best = v;
					}
				river[c] = best;
			}
	#undef NBR
	free(L);
}

static unsigned char toByte(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (unsigned char)(v * 255.0f + 0.5f);
}

/* Uploads an RGBA8 map (climate / ocean layout) with its mip chain. GL:
   main thread only. */
/* Uploads a cube map (faces back to back, faceBytes each) with its mip
   chain. Seamless filtering is switched on for good: the body shaders
   sample across face edges. GL: main thread only. */
static GLuint uploadCube(GLenum internalFormat, GLenum format, GLenum type,
                         const void *px, size_t faceBytes)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (int f = 0; f < 6; f++)
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, internalFormat, FACE_N, FACE_N, 0,
		             format, type, (const unsigned char *)px + f * faceBytes);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	return tex;
}

static GLuint uploadRGBA8(const unsigned char *px)
{
	return uploadCube(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px, FACE_TEXELS * 4);
}

/* Radius in cube texels of a distance in reference texels (face centre). */
static int refToCubeTexels(double r)
{
	int t = (int)(r * REF_TEXEL_RAD / (2.0 / FACE_N) + 0.5);
	return t < 1 ? 1 : t;
}

/* Builds the RGBA8 climate map's pixels (layout in TerrainGen.h), one cube
   field each. hEq / anomEq: the height and anomaly on the equirect grid,
   for the moisture's latitude streams. CPU only: runs on the bake worker. */
static unsigned char *climatePixels(const PlanetStyle *ps, const float *h, const float *hEq,
                                    const float *anomEq, float *dep, float sea)
{
	size_t n = CUBE_TEXELS;
	float *moistEq = malloc((size_t)EQ_W * EQ_H * sizeof(float));
	computeMoisture(hEq, anomEq, sea, moistEq);
	float *moist = equirectToCube(moistEq);
	free(moistEq);
	float *river = malloc(n * sizeof(float));
	float *lake = malloc(n * sizeof(float));
	computeDrainage(ps, h, moist, sea, river, lake);

	/* Alluvium: soften, then normalize by its 99th area percentile. */
	static Histo dh;
	blurCube(dep, refToCubeTexels(2.0));
	histoBuildCube(dep, &dh);
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

	/* Debug: TERRAIN_DUMP_CLIMATE names a file for the raw RGBA8 map. */
	const char *path = getenv("TERRAIN_DUMP_CLIMATE");
	if (path && *path) {
		FILE *f = fopen(path, "wb");
		if (f) { fwrite(px, 1, n * 4, f); fclose(f); }
	}
	return px;
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
   water behind islands stay calm; windward coasts face a developed sea.
   The fetch runs on the equirect grid (hEq) and is resampled; the wind
   comes from each cube texel's own latitude and anomaly. */
static unsigned char *oceanPixels(const float *hEq, const float *anom, float sea)
{
	const int W = EQ_W, H = EQ_H;
	size_t n = (size_t)W * H;
	const float *h = hEq;
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

	unsigned char *px = malloc(CUBE_TEXELS * 4);
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < FACE_N; j++)
			for (int i = 0; i < FACE_N; i++) {
				size_t c = ((size_t)f * FACE_N + j) * FACE_N + i;
				double d[3];
				texelDir(f, i, j, d);
				double lat = asin(fmax(fmin(d[2], 1.0), -1.0)) * (180.0 / M_PI);
				float zonal, pole, speed;
				prevailingWind(fabs(lat), &zonal, &pole, &speed);
				float north = lat >= 0.0 ? pole : -pole;
				float len = sqrtf(zonal * zonal + north * north);
				float de = len > 1e-4f ? zonal / len : 1.0f;
				float dn = len > 1e-4f ? north / len : 0.0f;
				float u = speed * (1.0f + WIND_ANOMALY_AMT * anom[c]);
				px[c * 4 + 0] = toByte(u / WIND_MAX_MS);
				px[c * 4 + 1] = toByte(de * 0.5f + 0.5f);
				px[c * 4 + 2] = toByte(dn * 0.5f + 0.5f);
				px[c * 4 + 3] = toByte(sampleEquirect(fetch, d));
			}
	free(fetch);
	return px;
}

/* ---------------- Surface map (slope + ambient occlusion) ---------------- */

/* RGBA16F surface map (layout in TerrainGen.h): the field's slope as a
   body-local vector (so seamless filtering and mips average it correctly
   across face edges) and a horizon-based ambient occlusion. quad.frag reads
   it instead of differencing four height samples per pixel. */
#define AO_DIRS   8
#define AO_STEPS  6           /* samples at 1, 2, 4 ... 32 texels */

/* float -> IEEE half (round to nearest; finite input). */
static unsigned short toHalf(float f)
{
	union { float f; unsigned int u; } v;
	v.f = f;
	unsigned int sign = (v.u >> 16) & 0x8000u;
	int e = (int)((v.u >> 23) & 0xffu) - 127 + 15;
	unsigned int man = v.u & 0x7fffffu;
	if (e >= 31)
		return (unsigned short)(sign | 0x7c00u);
	if (e <= 0) {
		if (e < -10)
			return (unsigned short)sign;
		man |= 0x800000u;
		unsigned int t = (unsigned int)(14 - e);
		unsigned int hm = man >> t;
		if ((man >> (t - 1)) & 1u) hm++;
		return (unsigned short)(sign | hm);
	}
	unsigned int hv = sign | ((unsigned int)e << 10) | (man >> 13);
	if (man & 0x1000u) hv++;   /* a carry into the exponent is still right */
	return (unsigned short)hv;
}

typedef struct {
	const float *h, *relief;
	unsigned short *px;
	int face;
} SurfaceTask;

/* One face of the surface map. Slope: central differences in face
   coordinates (neighbours across the edges), turned into the body-local
   tangent vector g with g . dd/dsc = dh/dsc and g . dd/dtc = dh/dtc (raw
   height units per radian, like quad.frag's old differences). AO: in
   AO_DIRS directions, the highest horizon within 32 texels over the
   relief (body radii, sphere curvature included); a horizon at tan t
   leaves the cosine-weighted sky fraction 1 / (1 + t^2). */
static unsigned __stdcall surfaceFace(void *arg)
{
	const SurfaceTask *task = arg;
	const int N = FACE_N, f = task->face;
	const float *h = task->h, *rel = task->relief;
	static const int DX[AO_DIRS] = { 1, 1, 0, -1, -1, -1, 0, 1 };
	static const int DY[AO_DIRS] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	const double ds = 2.0 / N;   /* face-coordinate step per texel */
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++) {
			size_t c = ((size_t)f * N + j) * N + i;
			int edge = i == 0 || j == 0 || i == N - 1 || j == N - 1;
			double hs = (h[edge ? cubeIndex(N, f, i + 1, j) : c + 1]
			           - h[edge ? cubeIndex(N, f, i - 1, j) : c - 1]) / (2.0 * ds);
			double ht = (h[edge ? cubeIndex(N, f, i, j + 1) : c + N]
			           - h[edge ? cubeIndex(N, f, i, j - 1) : c - N]) / (2.0 * ds);
			double sc = texelToFace(i, N), tc = texelToFace(j, N);
			double q[3], d[3], e1[3], e2[3];
			faceDir(f, sc, tc, q);
			double r = sqrt(dot3(q, q));
			for (int k = 0; k < 3; k++) d[k] = q[k] / r;
			double dS = dot3(d, faceS[f]), dT = dot3(d, faceT[f]);
			for (int k = 0; k < 3; k++) {
				e1[k] = (faceS[f][k] - d[k] * dS) / r;
				e2[k] = (faceT[f][k] - d[k] * dT) / r;
			}
			double aa = dot3(e1, e1), ab = dot3(e1, e2), bb = dot3(e2, e2);
			double inv = 1.0 / (aa * bb - ab * ab);
			double ca = (bb * hs - ab * ht) * inv, cb = (aa * ht - ab * hs) * inv;

			size_t m = (size_t)j * N + i;
			double sp = 0.5 * (texSpS[m] + texSpT[m]) * REF_TEXEL_RAD;   /* radians per texel */
			double r0 = rel[c], vis = 0.0;
			int inside = i >= 32 && j >= 32 && i < N - 32 && j < N - 32;
			for (int a = 0; a < AO_DIRS; a++) {
				double len = (DX[a] && DY[a]) ? M_SQRT2 : 1.0, tmax = 0.0;
				for (int s = 0, dist = 1; s < AO_STEPS; s++, dist *= 2) {
					size_t q2 = inside ? c + (ptrdiff_t)DY[a] * dist * N + DX[a] * dist
					                   : cubeIndex(N, f, i + DX[a] * dist, j + DY[a] * dist);
					double dl = dist * len * sp;
					double t = (rel[q2] - r0 - 0.5 * dl * dl) / dl;
					if (t > tmax) tmax = t;
				}
				vis += 1.0 / (1.0 + tmax * tmax);
			}

			unsigned short *o = task->px + c * 4;
			for (int k = 0; k < 3; k++)
				o[k] = toHalf((float)(ca * e1[k] + cb * e2[k]));
			o[3] = toHalf((float)(vis / AO_DIRS));
		}
	return 0;
}

/* Builds the surface map's half-float pixels, one thread per face. Uses
   the baked planet's relief (re-bake after changing landRelief /
   oceanRelief to update the occlusion). stepRms: RMS height step between
   neighbouring land texels (raw units), the detail noise's reference. */
static unsigned short *surfacePixels(const PlanetStyle *ps, const float *h, float sea,
                                     float oceanDepth, float *stepRms)
{
	float *rel = malloc(CUBE_TEXELS * sizeof(float));
	unsigned short *px = malloc(CUBE_TEXELS * 4 * sizeof(unsigned short));
	double landScale = ps->landRelief / fmax(1.0 - sea, 1e-3);
	double sum2 = 0.0;
	size_t cnt = 0;
	for (size_t c = 0; c < CUBE_TEXELS; c++) {
		double s = fmin(fmax((sea - h[c]) / fmax(oceanDepth, 1e-4), 0.0), 1.0);
		rel[c] = (float)(landScale * fmax(h[c] - sea, 0.0) - ps->oceanRelief * s * s);
		if ((c % FACE_N) + 1 < FACE_N && h[c] >= sea && h[c + 1] >= sea) {
			double dh = h[c + 1] - h[c];
			sum2 += dh * dh;
			cnt++;
		}
	}
	*stepRms = cnt ? (float)sqrt(sum2 / cnt) : 0.0f;

	SurfaceTask tasks[6];
	HANDLE th[6];
	for (int f = 0; f < 6; f++) {
		tasks[f].h = h; tasks[f].relief = rel; tasks[f].px = px; tasks[f].face = f;
		th[f] = (HANDLE)_beginthreadex(NULL, 0, surfaceFace, &tasks[f], 0, NULL);
		if (!th[f])
			surfaceFace(&tasks[f]);   /* no thread: do it here */
	}
	for (int f = 0; f < 6; f++)
		if (th[f]) {
			WaitForSingleObject(th[f], INFINITE);
			CloseHandle(th[f]);
		}
	free(rel);
	return px;
}

void setOceanUniforms(unsigned int shader, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g_planetGen.oceanTex);
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
	glBindTexture(GL_TEXTURE_CUBE_MAP, g_planetGen.climateTex);
	glUniform1i(glGetUniformLocation(shader, "climateMap"), unit);
	glActiveTexture(GL_TEXTURE0 + TERRAIN_SURFACE_UNIT);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g_planetGen.surfaceTex);
	glUniform1i(glGetUniformLocation(shader, "surfaceMap"), TERRAIN_SURFACE_UNIT);
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
   there as raw float32 (the six N x N faces back to back, GL face order and
   orientation) for offline inspection. TERRAIN_DUMP_CLIMATE likewise. */
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

/* ---------------- Height bounds pyramid ----------------
   Min/max mip pyramid of the final field, per cube face, for the
   quadtree's per-node culling and split distances. Level k texel = min/max
   of the 2x2 block of level k-1: the same footprint glGenerateMipmap
   averages, so any mip sample the shaders take lies inside the [min, max]
   of its block (or, near a face edge, of its block and the neighbouring
   face's, which seamless filtering blends in). */
#define PYR_MAX_LEVELS 16
struct TerrainPyramid {
	int levels;
	int n[PYR_MAX_LEVELS];                             /* texels per face edge */
	float *mn[PYR_MAX_LEVELS], *mx[PYR_MAX_LEVELS];   /* six faces; level 0: mn == mx == the field */
};

static void freePyramid(TerrainPyramid *p)
{
	if (!p)
		return;
	for (int k = 0; k < p->levels; k++) {
		free(p->mn[k]);
		if (k > 0)
			free(p->mx[k]);
	}
	free(p);
}

void TerrainGen_FreePlanet(PlanetGenParams *p)
{
	if (p->climateTex) glDeleteTextures(1, &p->climateTex);
	if (p->oceanTex) glDeleteTextures(1, &p->oceanTex);
	if (p->surfaceTex) glDeleteTextures(1, &p->surfaceTex);
	freePyramid(p->pyramid);
	memset(p, 0, sizeof *p);
}

/* Takes ownership of h (the uploaded cube field). */
static TerrainPyramid *buildPyramid(float *h)
{
	TerrainPyramid *pp = calloc(1, sizeof *pp);
	pp->n[0] = FACE_N;
	pp->mn[0] = pp->mx[0] = h;
	pp->levels = 1;
	while (pp->n[pp->levels-1] > 1 && pp->levels < PYR_MAX_LEVELS) {
		int k = pp->levels;
		int pn = pp->n[k-1], n = pn / 2;
		float *mn = malloc((size_t)6 * n * n * sizeof(float));
		float *mx = malloc((size_t)6 * n * n * sizeof(float));
		for (int f = 0; f < 6; f++) {
			const float *pmn = pp->mn[k-1] + (size_t)f * pn * pn;
			const float *pmx = pp->mx[k-1] + (size_t)f * pn * pn;
			float *omn = mn + (size_t)f * n * n, *omx = mx + (size_t)f * n * n;
			for (int y = 0; y < n; y++)
				for (int x = 0; x < n; x++) {
					size_t a = (size_t)(2*y) * pn + 2*x, c = a + pn;
					omn[(size_t)y*n + x] = fminf(fminf(pmn[a], pmn[a+1]), fminf(pmn[c], pmn[c+1]));
					omx[(size_t)y*n + x] = fmaxf(fmaxf(pmx[a], pmx[a+1]), fmaxf(pmx[c], pmx[c+1]));
				}
		}
		pp->n[k] = n;
		pp->mn[k] = mn; pp->mx[k] = mx;
		pp->levels++;
	}
	return pp;
}

/* The active planet's pyramid (an empty one before the first bake). */
static const TerrainPyramid *activePyramid(void)
{
	static const TerrainPyramid empty;
	return g_planetGen.pyramid ? g_planetGen.pyramid : &empty;
}

int TerrainGen_GlobalRange(float *hmin, float *hmax)
{
	return TerrainGen_PlanetRange(&g_planetGen, hmin, hmax);
}

int TerrainGen_PlanetRange(const PlanetGenParams *planet, float *hmin, float *hmax)
{
	static const TerrainPyramid empty;
	const TerrainPyramid *P = planet->pyramid ? planet->pyramid : &empty;
	if (P->levels == 0) {
		*hmin = *hmax = 0.0f;
		return 0;
	}
	int top = P->levels - 1;   /* one texel per face */
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (int f = 0; f < 6; f++) {
		lo = fminf(lo, P->mn[top][f]);
		hi = fmaxf(hi, P->mx[top][f]);
	}
	*hmin = lo;
	*hmax = hi;
	return 1;
}

/* Box of face coordinates (sc0, sc1, tc0, tc1) covering a spherical cap
   (unit centre c, angle cap) on face f; 0 if the cap misses the face. The
   angle of a point from the face centre toward S, seen around the T axis,
   is a longitude about T: over a cap it spans asin(sin(cap) / cos(lat))
   either side of the centre's (lat measured from the T pole), and
   sc = tan of it. Likewise tc, about S. */
static int capFaceBox(const double c[3], double cap, int f, double box[4])
{
	for (int a = 0; a < 2; a++) {
		const double *U = a ? faceT[f] : faceS[f];   /* the coordinate's axis */
		const double *Q = a ? faceS[f] : faceT[f];   /* the pole it turns about */
		double lat = asin(fmax(fmin(dot3(c, Q), 1.0), -1.0));
		double lo = -0.25 * M_PI, hi = 0.25 * M_PI;
		if (cap + fabs(lat) < 0.5 * M_PI) {
			double lon = atan2(dot3(c, U), dot3(c, faceM[f]));
			double dl = asin(fmin(sin(cap) / cos(lat), 1.0));
			lo = fmax(lo, lon - dl);
			hi = fmin(hi, lon + dl);
			if (lo > hi)
				return 0;
		}
		box[2*a] = tan(lo);
		box[2*a + 1] = tan(hi);
	}
	return 1;
}

int TerrainGen_HeightRange(const double dir[3], double capAngle, double lod, float *hmin, float *hmax)
{
	const TerrainPyramid *P = activePyramid();
	if (P->levels == 0) {
		*hmin = *hmax = 0.0f;
		return 0;
	}

	/* The shaders move the mip toward the face corners, where texels are
	   smaller (planetmap.glsl planetMapLodBias: -log2(m^2), m = the
	   direction's major component): add the most the cap can get. */
	double len = sqrt(dot3(dir, dir));
	double c[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
	double mC = fmax(fabs(c[0]), fmax(fabs(c[1]), fabs(c[2])));
	double aMax = fmin(acos(fmin(mC, 1.0)) + capAngle, acos(1.0 / sqrt(3.0)));
	double mMin = cos(aMax);
	lod += -log2(mMin * mMin);

	double box[6][4];
	int touched[6];
	for (int f = 0; f < 6; f++)
		touched[f] = capFaceBox(c, capAngle, f, box[f]);

	/* Mip 0 only (every deep node at planet scale): the sampled surface is
	   exactly the bilinear interpolation of the texels, whose extremes over
	   a box lie at the box corners, where its edges cross texel rows /
	   columns, or at texel centres inside it. Evaluating just those gives
	   the exact range: tight bounds for nodes far smaller than a texel.
	   Texels off the face (the half-texel strip along an edge) are the
	   neighbouring face's, as with seamless filtering. */
	if (lod <= 0.0) {
		const int N = P->n[0];
		int tiny = 1;
		for (int f = 0; f < 6 && tiny; f++)
			if (touched[f])
				tiny = (box[f][1] - box[f][0]) * 0.5 * N <= 16.0
				     && (box[f][3] - box[f][2]) * 0.5 * N <= 16.0;
		if (tiny) {
			const float *fld = P->mn[0];
			float lo = FLT_MAX, hi = -FLT_MAX;
			for (int f = 0; f < 6; f++) {
				if (!touched[f]) continue;
				double xa = faceToTexel(box[f][0], N), xb = faceToTexel(box[f][1], N);
				double ya = faceToTexel(box[f][2], N), yb = faceToTexel(box[f][3], N);
				double xs[20], ys[20];
				int nx = 0, ny = 0;
				xs[nx++] = xa;
				for (double g = floor(xa) + 1.0; g < xb; g += 1.0) xs[nx++] = g;
				xs[nx++] = xb;
				ys[ny++] = ya;
				for (double g = floor(ya) + 1.0; g < yb; g += 1.0) ys[ny++] = g;
				ys[ny++] = yb;
				for (int j = 0; j < ny; j++) {
					int y0 = (int)floor(ys[j]);
					double fy = ys[j] - y0;
					for (int i = 0; i < nx; i++) {
						int x0 = (int)floor(xs[i]);
						double fx = xs[i] - x0;
						double h00 = fld[cubeIndex(N, f, x0, y0)], h10 = fld[cubeIndex(N, f, x0 + 1, y0)];
						double h01 = fld[cubeIndex(N, f, x0, y0 + 1)], h11 = fld[cubeIndex(N, f, x0 + 1, y0 + 1)];
						double a = h00 + fx * (h10 - h00), b = h01 + fx * (h11 - h01);
						float v = (float)(a + fy * (b - a));
						if (v < lo) lo = v;
						if (v > hi) hi = v;
					}
				}
			}
			/* Float uv/filtering differences on the GPU side. */
			float eps = 1e-6f * (hi - lo) + 1e-7f;
			*hmin = lo - eps;
			*hmax = hi + eps;
			return 1;
		}
	}

	/* Otherwise the min/max pyramid: at least the coarsest mip the shader
	   may read there (a coarser block's min/max bounds every finer sample
	   inside it), and coarse enough that each face's box spans a handful of
	   texels. */
	int top = P->levels - 1;
	int k = (int)ceil(fmax(lod, 0.0));
	if (k > top) k = top;
	for (;;) {
		int fits = 1;
		for (int f = 0; f < 6 && fits; f++)
			if (touched[f])
				fits = (box[f][1] - box[f][0]) * 0.5 * P->n[k] <= 8.0
				    && (box[f][3] - box[f][2]) * 0.5 * P->n[k] <= 8.0;
		if (fits || k == top) break;
		k++;
	}

	/* Texels bilinear filtering touches, plus one more each side so the
	   finer mip of a trilinear blend stays inside; off-face texels are the
	   neighbouring face's. */
	const int n = P->n[k];
	const float *mn = P->mn[k], *mx = P->mx[k];
	float lo = FLT_MAX, hi = -FLT_MAX;
	for (int f = 0; f < 6; f++) {
		if (!touched[f]) continue;
		int x0 = (int)floor(faceToTexel(box[f][0], n)) - 1, x1 = (int)floor(faceToTexel(box[f][1], n)) + 2;
		int y0 = (int)floor(faceToTexel(box[f][2], n)) - 1, y1 = (int)floor(faceToTexel(box[f][3], n)) + 2;
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				size_t i = cubeIndex(n, f, x, y);
				if (mn[i] < lo) lo = mn[i];
				if (mx[i] > hi) hi = mx[i];
			}
	}
	*hmin = lo;
	*hmax = hi;
	return 1;
}

/* ---------------- Bake pipeline ----------------
   Stages: shaping (the GPU pass and its readback), the CPU work (erosion,
   normalization, climate / ocean pixels, bounds pyramid) and the upload.
   A blocking bake runs them back to back. A background bake spreads the
   GPU pass over frames in horizontal strips, reads it back through a
   pixel buffer once a fence says it is done, runs the CPU work on a
   worker thread and uploads when polled after that: each frame only pays
   a strip, a copy or one texture upload. The CPU stage's scratch state (xsState, the
   histograms, the drainage queue) and the shaping target are file-static,
   so one bake runs at a time: a new one first completes the background
   job's GPU stages and waits for its worker. */
#define BAKE_SHAPE_STRIPS 32   /* background shaping: one strip per poll */

typedef enum { STAGE_SHAPE, STAGE_READBACK, STAGE_CPU, STAGE_UPLOAD } BakeStage;

struct TerrainBakeJob {
	TerrainBakeSettings bs;
	PlanetStyle style;            /* the baked planet's (not the active one's) */
	void (*step)(void);
	BakeStage stage;
	int strip;                    /* next strip to draw (STAGE_SHAPE) */
	GLuint pbo;                   /* readback buffer (STAGE_READBACK) */
	GLsync fence;
	float *rg;                    /* shaping readback (W x H, R = height, G = anomaly); the CPU stage frees it */
	/* CPU stage results, uploaded by the finish */
	TerrainPyramid *pyramid;      /* level 0 is the final field */
	unsigned char *climatePx, *oceanPx;
	unsigned short *surfacePx;    /* half floats */
	float seaLevel, oceanDepth, landStepRms;
	/* Upload results (STAGE_UPLOAD: one per poll) */
	GLuint climateTex, oceanTex, surfaceTex, heightTex;
	double uploadS;
	clock_t t[7];
	int background;
	HANDLE thread;                /* the worker, until joined */
	volatile LONG done;           /* CPU stage finished */
};

static TerrainBakeJob *bgJob;       /* the background job, until finished or cancelled */
static volatile LONG workerSteps;   /* its progress, in step() calls */

static void workerStep(void)
{
	InterlockedIncrement(&workerSteps);
}

/* Draws shaping strips [job->strip, end) into the working target. The
   shader's uniforms (seed, plates, shaping) were set by the first call and
   stay in the program between frames. */
static void shapeStrips(TerrainBakeJob *job, int end)
{
	const int W = FACE_N, H = 6 * FACE_N;
	if (job->strip == 0) {
		ensureBakeResources();
		job->t[0] = clock();
		glUseProgram(bakeProgram);
		glUniform1f(glGetUniformLocation(bakeProgram, "seed"), (float)job->bs.seed);
		glUniform1i(glGetUniformLocation(bakeProgram, "faceSize"), FACE_N);
		uploadPlates(&job->bs);
		uploadShaping(&job->bs);
	}
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glViewport(0, 0, W, H);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glUseProgram(bakeProgram);
	glBindVertexArray(bakeQuadVAO);
	for (; job->strip < end; job->strip++) {
		int y0 = H * job->strip / BAKE_SHAPE_STRIPS, y1 = H * (job->strip + 1) / BAKE_SHAPE_STRIPS;
		glScissor(0, y0, W, y1 - y0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, getWindowWidth(), getWindowHeight());
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	if (blend) glEnable(GL_BLEND);
	if (!scissor) glDisable(GL_SCISSOR_TEST);
}

#define READBACK_BYTES (CUBE_TEXELS * 2 * sizeof(float))

static float *allocRG(void)
{
	float *p = malloc(READBACK_BYTES);
	if (!p) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	return p;
}

/* Blocking readback of the whole target into job->rg (only the two
   channels the bake uses: half the copy). */
static void readbackNow(TerrainBakeJob *job)
{
	job->rg = allocRG();
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, FACE_N, 6 * FACE_N, GL_RG, GL_FLOAT, job->rg);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	job->t[1] = clock();
}

/* Starts an asynchronous readback into a pixel buffer, fenced. */
static void readbackStart(TerrainBakeJob *job)
{
	glGenBuffers(1, &job->pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, job->pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)READBACK_BYTES, NULL, GL_STREAM_READ);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, FACE_N, 6 * FACE_N, GL_RG, GL_FLOAT, (void *)0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	job->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
}

static void readbackRelease(TerrainBakeJob *job)
{
	if (job->fence) glDeleteSync(job->fence);
	if (job->pbo) glDeleteBuffers(1, &job->pbo);
	job->fence = 0;
	job->pbo = 0;
}

/* Copies a finished pixel-buffer readback into job->rg. wait: block for
   the fence. Returns 1 once copied. */
static int readbackCollect(TerrainBakeJob *job, int wait)
{
	GLenum r = glClientWaitSync(job->fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
	                            wait ? 10000000000ull : 0);
	if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
		return 0;
	job->rg = allocRG();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, job->pbo);
	const void *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)READBACK_BYTES, GL_MAP_READ_BIT);
	if (src)
		memcpy(job->rg, src, READBACK_BYTES);
	else
		memset(job->rg, 0, READBACK_BYTES);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	readbackRelease(job);
	job->t[1] = clock();
	return 1;
}

/* 2-7. Everything between the readback and the upload. No GL. */
static void bakeCPU(TerrainBakeJob *job)
{
	const TerrainBakeSettings *bs = &job->bs;
	const size_t n = CUBE_TEXELS;
	static Histo hf;   /* 32 KB: keep it off the stack */

	xsState = bs->seed * 2654435761u | 1u;

	/* 2. Split the readback (r = shaped h, g = climate anomaly). */
	float *h = malloc(n * sizeof(float));
	float *anom = malloc(n * sizeof(float));
	float *dep = calloc(n, sizeof(float));
	if (!h || !anom || !dep) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	for (size_t i = 0; i < n; i++) { h[i] = job->rg[i * 2]; anom[i] = job->rg[i * 2 + 1]; }
	free(job->rg);
	job->rg = NULL;
	clampFinite(h, n);
	clampFinite(anom, n);

	/* Pre-normalize so the erosion constants are in fixed [0,1] units. The
	   shaper's sea level is raw 0; track where it lands. */
	histoBuildCube(h, &hf);
	float preLo = histoPctl(&hf, PRE_LO_PCT), preHi = histoPctl(&hf, PRE_HI_PCT);
	stretch01(h, n, preLo, preHi);
	float seaLevel0 = (0.0f - preLo) / fmaxf(preHi - preLo, 1e-6f);
	job->t[2] = clock();

	/* 3. Thermal erosion (weathering), then 4. hydraulic droplet erosion. */
	thermalErode(h, bs->thermalPasses, bs->thermalTalus, job->step);
	job->t[3] = clock();

	/* Droplets spawn only on land; reaching the sea is a river mouth. */
	hydraulicErode(h, dep, seaLevel0, bs, job->step);
	job->t[4] = clock();

	/* 5. Percentile-stretch the relief to [0,1], carrying the sea level
	   (raw 0, set by the plate crust) through it, + ocean depth (95th area
	   percentile of depth below sea level, feeds the water.frag tint). */
	histoBuildCube(h, &hf);
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
		for (size_t i = 0; i < n; i++) {
			if (h[i] < seaLevel) {
				double w = texArea[i % FACE_TEXELS];
				oceanArea += w;
				int b = (int)((double)(seaLevel - h[i]) * dscale);
				dhist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)] += w;
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
	job->t[5] = clock();

	/* 7. Climate map (moisture, rivers, alluvium, lakes) and ocean map. The
	   latitude streams (moisture, fetch) run on an equirect copy. */
	float *hEq = cubeToEquirect(h), *anomEq = cubeToEquirect(anom);
	job->climatePx = climatePixels(&job->style, h, hEq, anomEq, dep, seaLevel);
	job->oceanPx = oceanPixels(hEq, anom, seaLevel);
	job->surfacePx = surfacePixels(&job->style, h, seaLevel, oceanDepth, &job->landStepRms);
	free(hEq);
	free(anomEq);
	free(anom);
	free(dep);
	job->seaLevel = seaLevel;
	job->oceanDepth = oceanDepth;
	/* Keeps h as level 0 of the quadtree's height-bounds pyramid. */
	job->pyramid = buildPyramid(h);
	job->t[6] = clock();
}

static unsigned __stdcall bakeWorker(void *arg)
{
	TerrainBakeJob *job = arg;
	bakeCPU(job);
	InterlockedExchange(&job->done, 1);
	return 0;
}

/* The CPU stage of a background job: on a worker thread, below the render
   thread's priority. */
static void startWorker(TerrainBakeJob *job)
{
	job->stage = STAGE_CPU;
	job->thread = (HANDLE)_beginthreadex(NULL, 0, bakeWorker, job, CREATE_SUSPENDED, NULL);
	if (job->thread) {
		SetThreadPriority(job->thread, THREAD_PRIORITY_BELOW_NORMAL);
		ResumeThread(job->thread);
	} else {
		/* No thread: bake it here instead. */
		bakeCPU(job);
		job->done = 1;
	}
}

/* Advances a background job's main-thread stages; wait: run them all now. */
static void advanceJob(TerrainBakeJob *job, int wait)
{
	if (job->stage == STAGE_SHAPE) {
		shapeStrips(job, wait ? BAKE_SHAPE_STRIPS : job->strip + 1);
		if (job->strip < BAKE_SHAPE_STRIPS)
			return;
		readbackStart(job);
		job->stage = STAGE_READBACK;
		if (!wait)
			return;   /* collect it next time: the GPU is still busy */
	}
	if (job->stage == STAGE_READBACK) {
		if (!readbackCollect(job, wait))
			return;
		startWorker(job);
	}
}

/* Runs a background job to the end of its CPU stage. */
static void completeJob(TerrainBakeJob *job)
{
	advanceJob(job, 1);
	if (job->thread) {
		WaitForSingleObject(job->thread, INFINITE);
		CloseHandle(job->thread);
		job->thread = NULL;
	}
}

static TerrainBakeJob *newJob(const TerrainBakeSettings *bs, const PlanetStyle *style,
                              void (*step)(void))
{
	if (bgJob)
		completeJob(bgJob);   /* one bake at a time */
	TerrainBakeJob *job = calloc(1, sizeof *job);
	if (!job) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	job->bs = *bs;
	job->style = *style;
	job->step = step;
	return job;
}

static void freeJob(TerrainBakeJob *job)
{
	if (bgJob == job)
		bgJob = NULL;
	readbackRelease(job);
	free(job->rg);
	free(job->climatePx);
	free(job->oceanPx);
	free(job->surfacePx);
	freePyramid(job->pyramid);
	GLuint tex[4] = { job->climateTex, job->oceanTex, job->surfaceTex, job->heightTex };
	for (int k = 0; k < 4; k++)
		if (tex[k]) glDeleteTextures(1, &tex[k]);
	free(job);
}

static GLuint uploadHeight(const float *h)
{
	return uploadCube(GL_R32F, GL_RED, GL_FLOAT, h, FACE_TEXELS * sizeof(float));
}

/* 8. Upload: climate / ocean / surface maps and the final field (the same
   sampling params the body shaders expect) with full mip chains, one per
   call (all: the rest at once). R32F: half floats would round heights to ~100 m
   steps at Earth scale, and the CPU bounds pyramid must see exactly the
   values the GPU samples. Returns 1 when all three are up. */
static int uploadNext(TerrainBakeJob *job, int all)
{
	job->stage = STAGE_UPLOAD;
	do {
		clock_t u0 = clock();
		if (!job->climateTex) {
			job->climateTex = uploadRGBA8(job->climatePx);
			free(job->climatePx);
			job->climatePx = NULL;
		} else if (!job->oceanTex) {
			job->oceanTex = uploadRGBA8(job->oceanPx);
			free(job->oceanPx);
			job->oceanPx = NULL;
		} else if (!job->surfaceTex) {
			job->surfaceTex = uploadCube(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, job->surfacePx,
			                             FACE_TEXELS * 4 * sizeof(unsigned short));
			free(job->surfacePx);
			job->surfacePx = NULL;
		} else if (!job->heightTex) {
			job->heightTex = uploadHeight(job->pyramid->mn[0]);
		}
		job->uploadS += (double)(clock() - u0) / CLOCKS_PER_SEC;
	} while (all && !job->heightTex);
	return job->heightTex != 0;
}

/* Hands the uploaded planet to out (the pyramid moves there) and frees the
   job. Returns the height texture. */
static GLuint bakeFinish(TerrainBakeJob *job, PlanetGenParams *out)
{
	uploadNext(job, 1);
	out->climateTex = job->climateTex;
	out->oceanTex = job->oceanTex;
	out->surfaceTex = job->surfaceTex;
	out->seaLevel = job->seaLevel;
	out->oceanDepth = job->oceanDepth;
	out->landStepRms = job->landStepRms;
	out->pyramid = job->pyramid;
	job->pyramid = NULL;
	GLuint heightTex = job->heightTex;
	job->climateTex = job->oceanTex = job->surfaceTex = job->heightTex = 0;

	const clock_t *t = job->t;
	double s = (double)(CLOCKS_PER_SEC);
	fprintf(stderr, "[terrain] bake seed %u%s: shape+readback %.3f s, split %.2f s, thermal %.2f s, hydro %.2f s, sea %.2f s, climate %.2f s, upload %.3f s -> seaLevel %.4f, oceanDepth %.4f, land step %.5f\n",
	        job->bs.seed, job->background ? " (background)" : "",
	        (double)(t[1] - t[0]) / s, (double)(t[2] - t[1]) / s, (double)(t[3] - t[2]) / s,
	        (double)(t[4] - t[3]) / s, (double)(t[5] - t[4]) / s, (double)(t[6] - t[5]) / s,
	        job->uploadS, job->seaLevel, job->oceanDepth, job->landStepRms);

	freeJob(job);
	return heightTex;
}

unsigned int generatePlanetHeightmap(const TerrainBakeSettings *bs, const PlanetStyle *style,
                                     PlanetGenParams *out, void (*step)(void))
{
	TerrainBakeJob *job = newJob(bs, style, step);
	shapeStrips(job, BAKE_SHAPE_STRIPS);
	readbackNow(job);
	bakeCPU(job);
	GLuint heightTex = bakeFinish(job, out);
	/* The new planet becomes the active one. */
	g_planetGen = *out;
	return heightTex;
}

TerrainBakeJob *TerrainGen_BakeStart(const TerrainBakeSettings *bs, const PlanetStyle *style)
{
	TerrainBakeJob *job = newJob(bs, style, workerStep);
	job->background = 1;
	InterlockedExchange(&workerSteps, 0);
	bgJob = job;
	advanceJob(job, 0);
	return job;
}

int TerrainGen_BakePoll(TerrainBakeJob *job)
{
	if (job->stage < STAGE_CPU)
		advanceJob(job, 0);
	else if (job->done) {
		if (job->thread) {
			CloseHandle(job->thread);   /* finished: nothing to wait for */
			job->thread = NULL;
		}
		return uploadNext(job, 0);
	}
	return 0;
}

float TerrainGen_BakeProgress(const TerrainBakeJob *job)
{
	/* Shaping ~5%; the steps cover the erosion, the climate maps one more. */
	if (job->stage < STAGE_CPU)
		return 0.05f * (float)job->strip / (float)BAKE_SHAPE_STRIPS;
	if (job->done)
		return 1.0f;
	float p = 0.05f + 0.95f * (float)workerSteps / (float)(TERRAIN_BAKE_STEPS + 1);
	return p < 1.0f ? p : 1.0f;
}

unsigned int TerrainGen_BakeFinish(TerrainBakeJob *job, PlanetGenParams *out)
{
	completeJob(job);
	return bakeFinish(job, out);
}

void TerrainGen_BakeCancel(TerrainBakeJob *job)
{
	if (job->thread) {
		WaitForSingleObject(job->thread, INFINITE);
		CloseHandle(job->thread);
		job->thread = NULL;
	}
	freeJob(job);
}
