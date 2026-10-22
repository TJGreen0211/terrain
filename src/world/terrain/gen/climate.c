/* Climate map (moisture, rivers, lakes) and ocean map (prevailing wind,
   fetch). */
#include "world/terrain/gen/gen_internal.h"

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

/* Radius in cube texels of a distance in reference texels (face centre). */
static int refToCubeTexels(double r)
{
	int t = (int)(r * REF_TEXEL_RAD / (2.0 / FACE_N) + 0.5);
	return t < 1 ? 1 : t;
}

/* Builds the RGBA8 climate map's pixels (layout in terrain_gen.h), one cube
   field each. hEq / anomEq: the height and anomaly on the equirect grid,
   for the moisture's latitude streams. CPU only: runs on the bake worker. */
unsigned char *climatePixels(const PlanetStyle *ps, const float *h, const float *hEq,
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

/* RGBA8 ocean map (layout in terrain_gen.h). Fetch streams along each
   latitude row with the zonal wind, like the moisture: open water adds its
   length, land resets it, two laps converge the wrap. Lee coasts and the
   water behind islands stay calm; windward coasts face a developed sea.
   The fetch runs on the equirect grid (hEq) and is resampled; the wind
   comes from each cube texel's own latitude and anomaly. */
unsigned char *oceanPixels(const float *hEq, const float *anom, float sea)
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


void TerrainGen_SetOceanUniforms(unsigned int shader, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g_planetGen.oceanTex);
	glUniform1i(glGetUniformLocation(shader, "oceanMap"), unit);
	glUniform1f(glGetUniformLocation(shader, "windMax"), WIND_MAX_MS);
	glActiveTexture(GL_TEXTURE0);
}
