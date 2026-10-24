#ifndef GEN_INTERNAL_H
#define GEN_INTERNAL_H
/* Private to world/terrain/gen/: the bake's shared layout, constants,
   cube geometry helpers and the functions and state its files share.
   The public API is world/terrain/terrain_gen.h. */
#include "engine/core/gl.h"
#include "engine/gfx/shader.h"
#include "engine/gfx/buffer.h"
#include "engine/platform/window.h"
#include "world/terrain/terrain_gen.h"
#include "world/WorldConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include <process.h>

/* The project builds without /O2; the bake's CPU erosion needs real
   optimization to stay inside the 1-3 s startup budget. Scoped to the
   bake's translation units (every gen/ file includes this header) so no
   other module's codegen changes. */
#pragma optimize("gtp", on)

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
#define STRETCH_KNEE         0.85f   /* stretch01: soft roll-off above this, not a clip */
#define OCEAN_DEPTH_PCT      0.95    /* depth normalizing percentile */

/* ---- Tectonic plates (shaping input for heightbake.frag) ---- */
/* Plate count and continental area: g_terrainBake (below). */
#define PLATE_MIN_SEP        0.45    /* min angle between plate centres (rad) */
#define PLATE_MARGIN_MAX     0.10f   /* passive-margin pullback range (rad) */

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

/* cubemap.c */
void clampFinite(float *buf, size_t n);
extern float rowCos[EQ_H];
void initRowCos(int H);
extern float *texArea, *texSpS, *texSpT;
extern float texAreaMax;
void initCubeMetric(void);
float sampleEquirect(const float *field, const double d[3]);
float *cubeToEquirect(const float *field);
float *equirectToCube(const float *eq);
void histoBuildCube(const float *buf, Histo *Hs);
void histoBuildEq(const float *buf, Histo *Hs);
float histoPctl(const Histo *Hs, double p);
void stretch01(float *h, size_t n, float lo, float hi);

/* erosion.c */
void thermalErode(float *h, int passes, float talus, void (*step)(void));
extern unsigned int xsState;
void initBrush(void);
void hydraulicErode(float *h, float *dep, float seaLevel0,
                    const TerrainBakeSettings *bs, void (*step)(void));

/* climate.c */
unsigned char *climatePixels(const PlanetStyle *ps, const float *h, const float *hEq,
                             const float *anomEq, float *dep, float sea);
unsigned char *oceanPixels(const float *hEq, const float *anom, float sea);

/* stats.c: the final field's relief statistics (TerrainReliefStats).
   climate: the climate map's pixels (RGBA8, A = lake depth), so the
   landmarks stay off lakes. */
void reliefStats(const float *h, float sea, const unsigned char *climate, TerrainReliefStats *out);

/* surface.c */
unsigned short *surfacePixels(const PlanetStyle *ps, const float *h, float sea,
                              float oceanDepth, float *stepRms);
extern PlanetStyle g_planetStyle;

/* upload.c */
extern unsigned int bakeProgram, bakeQuadVAO, workFBO, workTex;
void ensureBakeResources(void);
GLuint uploadCube(GLenum internalFormat, GLenum format, GLenum type,
                  const void *px, size_t faceBytes);
GLuint uploadRGBA8(const unsigned char *px);
void uploadPlates(const TerrainBakeSettings *bs);
void uploadShaping(const TerrainBakeSettings *bs);
GLuint uploadHeight(const float *h);

/* bounds.c */
void freePyramid(TerrainPyramid *p);
TerrainPyramid *buildPyramid(float *h);

/* bake_job.c */
extern PlanetGenParams g_planetGen;

#endif
