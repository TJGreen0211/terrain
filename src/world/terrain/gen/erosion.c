/* Thermal (talus) and hydraulic (droplet) erosion of the cube field. */
#include "world/terrain/gen/gen_internal.h"

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
void thermalErode(float *h, int passes, float talus, void (*step)(void))
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
unsigned int xsState;
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

void initBrush(void)
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
void hydraulicErode(float *h, float *dep, float seaLevel0,
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
