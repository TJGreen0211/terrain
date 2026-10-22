/* CPU height field helpers, cube map texel metric and sampling, equirect
   conversion, area-weighted percentiles. */
#include "world/terrain/gen/gen_internal.h"

/* ---------------- CPU height field helpers ---------------- */

/* Replace NaN/Inf so no poison reaches histograms or the mip chain. */
void clampFinite(float *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float v = buf[i];
		if (!(v == v)) v = 0.0f;
		else if (v > FLT_MAX) v = FLT_MAX;
		else if (v < -FLT_MAX) v = -FLT_MAX;
		buf[i] = v;
	}
}

float rowCos[EQ_H];

/* cos(latitude) of every texel row's centre: the row's area weight. */
void initRowCos(int H)
{
	for (int y = 0; y < H; y++) {
		double lat = (((double)y + 0.5) / (double)H - 0.5) * M_PI;
		float c = (float)cos(lat);
		rowCos[y] = c < MIN_COS_LAT ? MIN_COS_LAT : c;
	}
}

/* Per-texel metric, the same on every face (index j*N + i): area = solid
   angle and spS / spT = centre spacing to the next texel along s / t, in
   reference-texel units (REF_TEXEL_RAD, its square for the area). The
   plain projection's texels shrink toward the face corners, to ~1/3 of
   the centre's area. */
float *texArea, *texSpS, *texSpT;
float texAreaMax;

void initCubeMetric(void)
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
float sampleEquirect(const float *field, const double d[3])
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
float *cubeToEquirect(const float *field)
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
float *equirectToCube(const float *eq)
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

void histoBuildCube(const float *buf, Histo *Hs)
{
	histoBuild(buf, CUBE_TEXELS, texArea, 1, FACE_TEXELS, Hs);
}

void histoBuildEq(const float *buf, Histo *Hs)
{
	histoBuild(buf, (size_t)EQ_W * EQ_H, rowCos, EQ_W, EQ_H, Hs);
}

float histoPctl(const Histo *Hs, double p)
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
void stretch01(float *h, size_t n, float lo, float hi)
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
