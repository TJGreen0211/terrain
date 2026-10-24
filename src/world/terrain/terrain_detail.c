#include "engine/platform/window.h"
#include "engine/gfx/shader.h"
#include "world/terrain/terrain_detail.h"
#include "world/terrain/terrain_gen.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static TerrainDetailParams detailParams = { 1, TERRAIN_DETAIL_ROUGHNESS, 1.0f, 1.0f };

TerrainDetailParams * TerrainDetail_Params(void)
{
	return &detailParams;
}

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

/* Gradient component c (axis) at table point (x, y, z), as RG8_SNORM: a
   random unit 3D vector (uniform on the sphere), so the two components a
   face uses are isotropic in direction. Independent uniform components
   fill a cube instead, biased toward the diagonals: the noise's features,
   and the ridged octaves' crests most of all, line up with the lattice
   (chevrons and blocks on steep rugged ground). */
static signed char gradComponent(int x, int y, int z, int c)
{
	double u = (hash3(x, y, z, 0) >> 8) * (2.0 / 16777215.0) - 1.0;
	double phi = (hash3(x, y, z, 1) >> 8) * (2.0 * M_PI / 16777215.0);
	double r = sqrt(fmax(1.0 - u * u, 0.0));
	double g = c == 0 ? r * cos(phi) : c == 1 ? r * sin(phi) : u;
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

/* Procedural ground textures (shaders/groundtex.frag, used by quad.frag):
   rock, ground, sand and snow as one RGBA16F 2D array, GROUND_TEX_SIZE per
   tile with mips, baked once on the GPU. */
#define GROUND_TEX_SIZE 512
#define GROUND_LAYERS 4
#define GROUND_TEXTURE_UNIT 17
static GLuint groundTex;

void TerrainDetail_Init(void)
{
	if (groundTex)
		return;
	GLuint prog, vao, fbo;
	Shader_Create(&prog, "world/terrain/groundtex.vert", "world/terrain/groundtex.frag");
	glGenTextures(1, &groundTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, groundTex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA16F, GROUND_TEX_SIZE, GROUND_TEX_SIZE, GROUND_LAYERS,
	             0, GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

	GLint vp[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glGenVertexArrays(1, &vao);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, GROUND_TEX_SIZE, GROUND_TEX_SIZE);
	glUseProgram(prog);
	glUniform1f(glGetUniformLocation(prog, "size"), (float)GROUND_TEX_SIZE);
	glBindVertexArray(vao);
	for (int l = 0; l < GROUND_LAYERS; l++) {
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, groundTex, 0, l);
		glUniform1i(glGetUniformLocation(prog, "layer"), l);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(prog);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
}

/* Highest lattice level: integer node addressing in the shaders (shifts of
   int32) holds up to 2^30 cells across a face. */
#define DETAIL_MAX_LEVEL 30

typedef struct DetailOctaves {
	int first, count;      /* lattice level of the coarsest octave, octave count */
	double total;          /* summed amplitude, metres (the roughest spectrum) */
	double maxRatio;       /* largest amplitude / wavelength over the octaves */
} DetailOctaves;

/* The roughness spectrum's amplitude (m) at wavelength lambda (m) for A10
   (m) and H (detail.glsl's detailAmpAt). */
static double ampAt(double a10, double H, double lambda)
{
	double l = fmax(lambda, TERRAIN_ROUGH_LAMBDA_C_M);
	return a10 * pow(l * 1e-4, H) * (lambda / l);
}

/* Octave n has 2^n lattice cells across a cube face, i.e. a wavelength of
   ~2R / 2^n at the face centre (shorter toward the corners, by up to the
   1/|q| of the cube projection, which only shortens it: the bounds take
   the face centre's, the amplitudes there are the largest when H <= 1).
   The coarsest is the map's texel grid (n = log2 TERRAIN_MAP_SIZE): the
   bake carries the spectrum above it. Each octave's bound is the larger of
   the plains' and the mountains' spectrum there, without the plains' knee
   (it only lowers them). */
static DetailOctaves octavesFor(double radius)
{
	DetailOctaves o = { 0, 0, 0.0, 0.0 };
	if (!detailParams.enabled || radius <= 0.0)
		return o;
	int first = (int)lround(log2((double)TERRAIN_MAP_SIZE));
	int last = (int)floor(log2(2.0 * radius / TERRAIN_DETAIL_MIN_WAVELENGTH_M));
	if (first < 0)
		first = 0;
	if (last > DETAIL_MAX_LEVEL)
		last = DETAIL_MAX_LEVEL;
	o.first = first;
	o.count = last >= first ? last - first + 1 : 0;
	double k = detailParams.roughness;
	for (int i = 0; i < o.count; i++) {
		double lambda = ldexp(2.0 * radius, -(first + i));
		double amp = k * fmax(ampAt(TERRAIN_ROUGH_A10_PLAINS_M, TERRAIN_ROUGH_H_PLAINS, lambda),
		                      ampAt(TERRAIN_ROUGH_A10_MOUNTAIN_M, TERRAIN_ROUGH_H_MOUNTAIN, lambda));
		o.total += amp;
		o.maxRatio = fmax(o.maxRatio, amp / lambda);
	}
	return o;
}

void TerrainDetail_SetUniforms(GLuint shader, double radius, double trianglePx)
{
	DetailOctaves o = octavesFor(radius);
	double pix = 2.0 * tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0) / Window_Height();
	glUniform1i(glGetUniformLocation(shader, "detailOctave0"), o.first);
	glUniform1i(glGetUniformLocation(shader, "detailOctaves"), o.count);
	double k = detailParams.roughness;
	glUniform2f(glGetUniformLocation(shader, "detailA10"),
	            (float)(k * TERRAIN_ROUGH_A10_PLAINS_M), (float)(k * TERRAIN_ROUGH_A10_MOUNTAIN_M));
	glUniform2f(glGetUniformLocation(shader, "detailH"), (float)TERRAIN_ROUGH_H_PLAINS, (float)TERRAIN_ROUGH_H_MOUNTAIN);
	glUniform2f(glGetUniformLocation(shader, "detailKnee"),
	            (float)TERRAIN_ROUGH_KNEE_PLAINS_M, (float)TERRAIN_ROUGH_KNEE_MOUNTAIN_M);
	glUniform1f(glGetUniformLocation(shader, "detailHLong"), (float)TERRAIN_ROUGH_H_LONG);
	glUniform1f(glGetUniformLocation(shader, "detailErode"), (float)TERRAIN_DETAIL_ERODE);
	glUniform1f(glGetUniformLocation(shader, "detailLambdaC"), (float)TERRAIN_ROUGH_LAMBDA_C_M);
	glUniform2f(glGetUniformLocation(shader, "detailRugged"), (float)TERRAIN_RUGGED_SLOPE_LO, (float)TERRAIN_RUGGED_SLOPE_HI);
	glUniform2f(glGetUniformLocation(shader, "detailAltRugged"), (float)TERRAIN_RUGGED_ALT_LO, (float)TERRAIN_RUGGED_ALT_HI);
	/* The map's slope (raw units per radian) as rise / run: metres of
	   relief per raw unit over metres per radian (the radius cancels). */
	glUniform1f(glGetUniformLocation(shader, "detailSlopeToRise"),
	            (float)(TerrainGen_Style()->landRelief / fmax(1.0 - TerrainGen_GenParams()->seaLevel, 1e-3)));
	glUniform1f(glGetUniformLocation(shader, "terrainAO"), detailParams.ao);
	glUniform1f(glGetUniformLocation(shader, "detailRadius"), (float)radius);
	glUniform1f(glGetUniformLocation(shader, "detailPix"), (float)pix);
	glUniform1f(glGetUniformLocation(shader, "detailTriPx"), (float)trianglePx);
	if (!gradTex)
		createGradTexture();
	glActiveTexture(GL_TEXTURE0 + DETAIL_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, gradTex);
	glUniform1i(glGetUniformLocation(shader, "detailGradTex"), DETAIL_TEXTURE_UNIT);
	glActiveTexture(GL_TEXTURE0 + GROUND_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, groundTex);
	glUniform1i(glGetUniformLocation(shader, "groundTex"), GROUND_TEXTURE_UNIT);
	glUniform1f(glGetUniformLocation(shader, "groundTexStrength"), groundTex ? detailParams.texStrength : 0.0f);
	glActiveTexture(GL_TEXTURE0);
}

/* The noise stays within [-1, 1] and the coastal ramp within [0, 1]; the
   roughest spectrum is octavesFor's total. */
double TerrainDetail_Amplitude(double radius)
{
	return radius > 0.0 ? octavesFor(radius).total / radius : 0.0;
}

/* Where two patch levels meet, the coarse edge interpolates linearly what
   the fine side samples. The geometric fade keeps only octaves at least
   ~1.5 coarse vertex spacings long, and the interpolation error falls off
   with the square of the wavelength above that, so the seam sums to under
   ~0.5 x the steepest amplitude/wavelength ratio x the patch width; 1.5
   covers the shorter wavelengths near cube corners, and 1.8 the ridged
   octaves' steeper gradient (detail.glsl). */
double TerrainDetail_Seam(double radius)
{
	return 1.5 * 1.8 * octavesFor(radius).maxRatio;
}
