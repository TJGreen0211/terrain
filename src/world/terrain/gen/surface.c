/* Surface map (slope + ambient occlusion) and the live surface style. */
#include "world/terrain/gen/gen_internal.h"

/* ---------------- Surface map (slope + ambient occlusion) ---------------- */

/* RGBA16F surface map (layout in terrain_gen.h): the field's slope as a
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
unsigned short *surfacePixels(const PlanetStyle *ps, const float *h, float sea,
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

/* ---------------- Surface style ---------------- */

/* Earth-like defaults. Colours are sRGB (as a colour picker shows them);
   TerrainGen_SetSurfaceUniforms converts them to linear albedo for the shader.
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
	(float)(EARTH_LAND_RELIEF_M / EARTH_RADIUS_M),    /* landRelief */
	(float)(EARTH_OCEAN_RELIEF_M / EARTH_RADIUS_M),   /* oceanRelief */
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

void TerrainGen_SetSurfaceUniforms(unsigned int shader, int unit)
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
