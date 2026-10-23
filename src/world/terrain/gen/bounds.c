/* Height bounds pyramid and the range queries the quadtree culls with. */
#include "world/terrain/gen/gen_internal.h"

void freePyramid(TerrainPyramid *p)
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
TerrainPyramid *buildPyramid(float *h)
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

/* planetmap.glsl planetMapFine filters a 2x2 block that reaches into the
   coast band (seastate.glsl seaCoastBand, up to quad.frag's BEACH_OFF
   above seaLevel) with Catmull-Rom over the surrounding 4x4 texels, at any
   lod below 1 (so anywhere a fragment comes close enough). Its negative
   lobes overshoot the texels: per axis they weigh at most 0.125 (t = 1/2),
   so in 2D 2 * 0.125 * 1.125 of the texels' range. Unbounded, the water
   culled shoreline patches where the overshoot dips below sea level. */
#define COAST_BAND_TOP   0.012f
#define CR_OVERSHOOT     0.28125f

/* 0 (range untouched) when every block is above the band: bilinear. */
static int widenCoastFilter(float *lo, float *hi)
{
	if (*lo > g_planetGen.seaLevel + COAST_BAND_TOP)
		return 0;
	float over = CR_OVERSHOOT * (*hi - *lo);
	*lo -= over;
	*hi += over;
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
			/* The texels Catmull-Rom reaches: one beyond the bilinear 2x2. */
			float tlo = FLT_MAX, thi = -FLT_MAX;
			for (int f = 0; f < 6; f++) {
				if (!touched[f]) continue;
				double xa = faceToTexel(box[f][0], N), xb = faceToTexel(box[f][1], N);
				double ya = faceToTexel(box[f][2], N), yb = faceToTexel(box[f][3], N);
				for (int y = (int)floor(ya) - 1; y <= (int)floor(yb) + 2; y++)
					for (int x = (int)floor(xa) - 1; x <= (int)floor(xb) + 2; x++) {
						float v = fld[cubeIndex(N, f, x, y)];
						if (v < tlo) tlo = v;
						if (v > thi) thi = v;
					}
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
			lo -= eps;
			hi += eps;
			if (widenCoastFilter(&tlo, &thi)) {
				lo = fminf(lo, tlo);
				hi = fmaxf(hi, thi);
			}
			*hmin = lo;
			*hmax = hi;
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
	/* Those texels already cover Catmull-Rom's 4x4 reach. */
	widenCoastFilter(&lo, &hi);
	*hmin = lo;
	*hmax = hi;
	return 1;
}
