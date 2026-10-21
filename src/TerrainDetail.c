#include "Window.h"
#include "TerrainDetail.h"
#include "WorldConfig.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

int g_terrainDetail = 1;

/* detail.glsl's gradient table: DETAIL_TABLE^3 random 3D gradients, one
   2D array texture holding it three times, once per constant cube axis
   (layers axis*DETAIL_TABLE + that coordinate; texels = the other two
   coordinates in increasing order; RG = those two gradient components).
   Every face reads the same 3D values, so faces agree along cube edges. */
#define DETAIL_TABLE 64
#define DETAIL_TEXTURE_UNIT 11
static GLuint gradTex;

static uint32_t hash3(int x, int y, int z, int c)
{
	uint32_t h = (uint32_t)x * 0x8da6b343u + (uint32_t)y * 0xd8163841u
	           + (uint32_t)z * 0xcb1ab31fu + (uint32_t)c * 0x9e3779b9u;
	h ^= h >> 16; h *= 0x7feb352du;
	h ^= h >> 15; h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

/* Gradient component c (axis) at table point (x, y, z), as RG8_SNORM. */
static signed char gradComponent(int x, int y, int z, int c)
{
	double g = (hash3(x, y, z, c) >> 8) * (2.0 / 16777215.0) - 1.0;
	return (signed char)lround(g * 127.0);
}

static void createGradTexture(void)
{
	const int N = DETAIL_TABLE;
	signed char *texels = malloc((size_t)3 * N * N * N * 2);
	for (int ca = 0; ca < 3; ca++) {
		int a1 = ca == 0 ? 1 : 0, a2 = ca == 2 ? 1 : 2;
		for (int l = 0; l < N; l++)
			for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++) {
					int c[3];
					c[ca] = l; c[a1] = i; c[a2] = j;
					signed char *t = texels + ((((size_t)ca * N + l) * N + j) * N + i) * 2;
					t[0] = gradComponent(c[0], c[1], c[2], a1);
					t[1] = gradComponent(c[0], c[1], c[2], a2);
				}
	}
	glGenTextures(1, &gradTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, gradTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG8_SNORM, N, N, 3 * N, 0, GL_RG, GL_BYTE, texels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	free(texels);
}

/* Highest lattice level: integer node addressing in the shaders (shifts of
   int32) holds up to 2^30 cells across a face. */
#define DETAIL_MAX_LEVEL 30

typedef struct DetailOctaves {
	int first, count;      /* lattice level of the coarsest octave, octave count */
	double amp0, total;    /* coarsest and summed amplitude, metres */
	double maxRatio;       /* largest amplitude / wavelength over the octaves */
} DetailOctaves;

/* Octave n has 2^n lattice cells across a cube face, i.e. a wavelength of
   ~2R / 2^n at the face centre (shorter toward the corners, by up to the
   1/|q| of the cube projection). */
static DetailOctaves octavesFor(double radius)
{
	DetailOctaves o = { 0, 0, TERRAIN_DETAIL_AMPLITUDE_M, 0.0, 0.0 };
	if (!g_terrainDetail || radius <= 0.0)
		return o;
	int first = (int)lround(log2(2.0 * radius / TERRAIN_DETAIL_WAVELENGTH_M));
	int last = (int)floor(log2(2.0 * radius / TERRAIN_DETAIL_MIN_WAVELENGTH_M));
	if (first < 0)
		first = 0;
	if (last > DETAIL_MAX_LEVEL)
		last = DETAIL_MAX_LEVEL;
	o.first = first;
	o.count = last >= first ? last - first + 1 : 0;
	double amp = o.amp0;
	for (int i = 0; i < o.count; i++) {
		double lambda = ldexp(2.0 * radius, -(first + i));
		o.total += amp;
		o.maxRatio = fmax(o.maxRatio, amp / lambda);
		amp *= TERRAIN_DETAIL_GAIN;
	}
	return o;
}

void TerrainDetail_SetUniforms(GLuint shader, double radius, double trianglePx)
{
	DetailOctaves o = octavesFor(radius);
	double pix = 2.0 * tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0) / getWindowHeight();
	glUniform1i(glGetUniformLocation(shader, "detailOctave0"), o.first);
	glUniform1i(glGetUniformLocation(shader, "detailOctaves"), o.count);
	glUniform1f(glGetUniformLocation(shader, "detailAmp0"), (float)o.amp0);
	glUniform1f(glGetUniformLocation(shader, "detailGain"), (float)TERRAIN_DETAIL_GAIN);
	glUniform1f(glGetUniformLocation(shader, "detailAmpTotal"), (float)fmax(o.total, 1e-3));
	glUniform1f(glGetUniformLocation(shader, "detailPlains"), (float)TERRAIN_DETAIL_PLAINS);
	glUniform1f(glGetUniformLocation(shader, "detailRadius"), (float)radius);
	glUniform1f(glGetUniformLocation(shader, "detailPix"), (float)pix);
	glUniform1f(glGetUniformLocation(shader, "detailTriPx"), (float)trianglePx);
	if (!gradTex)
		createGradTexture();
	glActiveTexture(GL_TEXTURE0 + DETAIL_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, gradTex);
	glUniform1i(glGetUniformLocation(shader, "detailGradTex"), DETAIL_TEXTURE_UNIT);
	glActiveTexture(GL_TEXTURE0);
}

/* The noise stays within [-1, 1] and the envelope within [0, 1]. */
double TerrainDetail_Amplitude(double radius)
{
	return radius > 0.0 ? octavesFor(radius).total / radius : 0.0;
}

/* Where two patch levels meet, the coarse edge interpolates linearly what
   the fine side samples. The geometric fade keeps only octaves at least
   ~1.5 coarse vertex spacings long, and the interpolation error falls off
   with the square of the wavelength above that, so the seam sums to under
   ~0.5 x the steepest amplitude/wavelength ratio x the patch width; 1.5
   covers the shorter wavelengths near cube corners. */
double TerrainDetail_Seam(double radius)
{
	return 1.5 * octavesFor(radius).maxRatio;
}
