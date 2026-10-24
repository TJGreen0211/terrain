#include "world/terrain/gen/gen_internal.h"

/* The land's relief statistics (TerrainReliefStats, terrain_gen.h): height
   and slope distributions over the final field, and landmark directions
   for review views. Units that do not depend on the planet's size or its
   relief setting: heights in alt01, slopes in alt01 per radian of arc. */

#define ELEV_BINS  1024
/* Slopes: log-spaced bins from SLOPE_LO to SLOPE_HI alt01 per radian (at
   Earth's 2x relief, 1 alt01 / rad is a 0.28% grade; 1e5 is 280:1). */
#define SLOPE_BINS 512
#define SLOPE_LO   1e-3
#define SLOPE_HI   1e5
#define RUGGED_BASE 4   /* baseline index (16 texels: a range's flank, not one
                           cliff) the rugged spot is picked on */
#define RUGGED_ALT_LO 0.2f /* ... this high (not a coastal slope) */
#define RUGGED_ALT_HI 0.9f /* ... and not the top plateau's edge */
#define PLAIN_BASE  3   /* baseline index (8 texels) the plain is picked on */
#define PLAIN_ALT_LO 0.02f
#define PLAIN_ALT_HI 0.15f
#define PLAIN_LAT_SIN 0.7f /* within ~45 deg of the equator (daylight all year) */
#define PLAIN_PCT   0.25 /* the plain's slope: this quantile of the land's (the
                            flattest land is a lake, filled level) */

static const int baseTexels[TERRAIN_STAT_BASES] = { 1, 2, 4, 8, 16 };

static int slopeBin(double s)
{
	if (s <= SLOPE_LO) return 0;
	double t = log(s / SLOPE_LO) / log(SLOPE_HI / SLOPE_LO) * SLOPE_BINS;
	return t >= SLOPE_BINS - 1 ? SLOPE_BINS - 1 : (int)t;
}

/* The steeper of the slopes along i and j over k texels from texel c
   (land at both ends, inside the face; -1 if neither). */
static float slopeAt(const float *h, float sea, double inv, double arc, size_t c, int i, int j, int k, int *ends)
{
	float s = -1.0f;
	*ends = 0;
	if (i + k < FACE_N && h[c + k] >= sea) {
		s = fmaxf(s, (float)(fabs(h[c + k] - h[c]) * inv / arc));
		(*ends)++;
	}
	if (j + k < FACE_N && h[c + (size_t)k * FACE_N] >= sea) {
		s = fmaxf(s, (float)(fabs(h[c + (size_t)k * FACE_N] - h[c]) * inv / arc));
		(*ends)++;
	}
	return s;
}

static float slopeOfBin(int b)
{
	return (float)(SLOPE_LO * pow(SLOPE_HI / SLOPE_LO, (b + 0.5) / SLOPE_BINS));
}

/* The bin holding quantile p of count samples in hist (bins), as an index. */
static int quantileBin(const unsigned int *hist, int bins, double count, double p)
{
	double want = p * count, acc = 0.0;
	for (int b = 0; b < bins; b++) {
		acc += hist[b];
		if (acc >= want)
			return b;
	}
	return bins - 1;
}

/* Angle (rad) between the directions of texels (f, i, j) and (f, i2, j2). */
static double arcBetween(int f, int i, int j, int i2, int j2)
{
	double a[3], b[3];
	texelDir(f, i, j, a);
	texelDir(f, i2, j2, b);
	double c = dot3(a, b);
	return acos(c > 1.0 ? 1.0 : c);
}

void reliefStats(const float *h, float sea, const unsigned char *climate, TerrainReliefStats *out)
{
	static unsigned int elevHist[ELEV_BINS];
	static unsigned int slopeHist[TERRAIN_STAT_BASES][SLOPE_BINS];
	memset(elevHist, 0, sizeof elevHist);
	memset(slopeHist, 0, sizeof slopeHist);
	memset(out, 0, sizeof *out);
	double inv = 1.0 / fmax(1.0 - sea, 1e-3);
	double slopeN[TERRAIN_STAT_BASES] = { 0 };

	/* Each baseline's arc, measured about the face centre's row (texels
	   shrink toward the corners by up to ~1/3: the stats are per texel). */
	int mid = FACE_N / 2;
	for (int b = 0; b < TERRAIN_STAT_BASES; b++)
		out->baseRad[b] = (float)arcBetween(4, mid, mid, mid + baseTexels[b], mid);

	size_t land = 0;
	float top = -1.0f, rugged = -1.0f;
	size_t topAt = 0, ruggedAt = 0, plainAt = 0;
	for (int f = 0; f < 6; f++)
		for (int j = 0; j < FACE_N; j++)
			for (int i = 0; i < FACE_N; i++) {
				size_t c = ((size_t)f * FACE_N + j) * FACE_N + i;
				if (h[c] < sea)
					continue;
				land++;
				float a = (float)((h[c] - sea) * inv);
				int eb = (int)(a * (ELEV_BINS - 1) + 0.5f);
				elevHist[eb < 0 ? 0 : (eb > ELEV_BINS - 1 ? ELEV_BINS - 1 : eb)]++;
				if (h[c] > top) { top = h[c]; topAt = c; }
				for (int b = 0; b < TERRAIN_STAT_BASES; b++) {
					/* Along i and along j, inside the face, land at both ends. */
					int k = baseTexels[b];
					if (i + k < FACE_N && h[c + k] >= sea) {
						slopeHist[b][slopeBin(fabs(h[c + k] - h[c]) * inv / out->baseRad[b])]++;
						slopeN[b] += 1.0;
					}
					if (j + k < FACE_N && h[c + (size_t)k * FACE_N] >= sea) {
						slopeHist[b][slopeBin(fabs(h[c + (size_t)k * FACE_N] - h[c]) * inv / out->baseRad[b])]++;
						slopeN[b] += 1.0;
					}
				}
				int ends;
				float sr = slopeAt(h, sea, inv, out->baseRad[RUGGED_BASE], c, i, j, baseTexels[RUGGED_BASE], &ends);
				int kr = baseTexels[RUGGED_BASE];
				if (ends == 2 && a >= RUGGED_ALT_LO && a <= RUGGED_ALT_HI && sr > rugged && !climate[c * 4 + 3]
				    && !climate[(c + kr) * 4 + 3] && !climate[(c + (size_t)kr * FACE_N) * 4 + 3]) {
					rugged = sr;
					ruggedAt = c;
				}
			}

	if (!land)
		return;
	out->landShare = (float)((double)land / CUBE_TEXELS);
	static const double q[3] = { 0.5, 0.9, 0.99 };
	for (int k = 0; k < 3; k++)
		out->elev[k] = (float)quantileBin(elevHist, ELEV_BINS, (double)land, q[k]) / (ELEV_BINS - 1);
	out->elev[3] = (float)((top - sea) * inv);
	for (int b = 0; b < TERRAIN_STAT_BASES; b++)
		for (int k = 0; k < 3; k++)
			out->slope[b][k] = slopeN[b] > 0.0 ? slopeOfBin(quantileBin(slopeHist[b], SLOPE_BINS, slopeN[b], q[k])) : 0.0f;

	/* The plain: low land with land all round whose slope is the land's
	   PLAIN_PCT quantile (a second pass, now the quantile is known). */
	float want = slopeOfBin(quantileBin(slopeHist[PLAIN_BASE], SLOPE_BINS, slopeN[PLAIN_BASE], PLAIN_PCT));
	float bestGap = 1e30f;
	int k8 = baseTexels[PLAIN_BASE];
	for (int f = 0; f < 6; f++)
		for (int j = k8; j < FACE_N; j++)
			for (int i = k8; i < FACE_N; i++) {
				size_t c = ((size_t)f * FACE_N + j) * FACE_N + i;
				if (h[c] < sea || h[c - k8] < sea || h[c - (size_t)k8 * FACE_N] < sea || climate[c * 4 + 3])
					continue;
				float a = (float)((h[c] - sea) * inv);
				if (a < PLAIN_ALT_LO || a > PLAIN_ALT_HI)
					continue;
				double d[3];
				texelDir(f, i, j, d);
				if (fabs(d[2]) > PLAIN_LAT_SIN)
					continue;
				int ends;
				float s = slopeAt(h, sea, inv, out->baseRad[PLAIN_BASE], c, i, j, k8, &ends);
				if (ends == 2 && fabsf(s - want) < bestGap) {
					bestGap = fabsf(s - want);
					plainAt = c;
				}
			}

	size_t at[3] = { topAt, ruggedAt, plainAt };
	double *dst[3] = { out->peak, out->rugged, out->plain };
	for (int m = 0; m < 3; m++) {
		int f = (int)(at[m] / FACE_TEXELS);
		size_t r = at[m] % FACE_TEXELS;
		texelDir(f, (int)(r % FACE_N), (int)(r / FACE_N), dst[m]);
	}
}

void TerrainGen_ReliefReport(const TerrainReliefStats *s, double landRelief, double radius,
                             TerrainReliefReport *r)
{
	for (int k = 0; k < 4; k++)
		r->elevM[k] = s->elev[k] * landRelief * radius;
	for (int b = 0; b < TERRAIN_STAT_BASES; b++) {
		r->baseM[b] = s->baseRad[b] * radius;
		for (int k = 0; k < 3; k++)
			r->slope[b][k] = s->slope[b][k] * landRelief;
	}
}
