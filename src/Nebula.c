#define _CRT_SECURE_NO_WARNINGS
#include "Common.h"
#include "Nebula.h"
#include "Bodies.h"
#include "WorldConfig.h"
#include <stb_image_write.h>
#include <direct.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The default look: EVE-style teal/amber duotone, ridged filaments, a
   tilted galaxy band. A macro so the static initializer and
   nebulaDefaults() share one copy. */
#define NEBULA_DEFAULTS { \
	.seed = NEBULA_SEED_DEFAULT, \
	.nearWavelength = 0.15f, \
	.nearCoverage = 0.25f, \
	.nearDensity = 1.0f, \
	.nearSigma = 2.0f, \
	.farWavelength = 0.18f, \
	.farCoverage = 0.6f, \
	.farDensity = 1.0f, \
	.farSigma = 8.0f, \
	.turbulence = 0.6f, \
	.contrast = 3.0f, \
	.emission = 0.15f, \
	.dark = 0.6f, \
	.colA = { 0.05f, 0.45f, 0.55f }, \
	.colB = { 0.90f, 0.45f, 0.12f }, \
	.band = 0.6f, \
	.bandTilt = 60.0f, \
	.starDensity = 0.6f, \
	.starBrightness = 0.5f, \
	.scatter = 0.5f, \
	.lightCount = 5.0f, \
	.lightIntensity = 7.0f, \
	.lightRadius = 1.0f, \
	.lightClump = 0.5f, \
	.dustAlbedo = 0.7f, \
	.ambient = 0.02f, \
	.phaseG = 0.75f, \
	.powder = 0.2f, \
	.coreSize = 0.004f, \
	.farErosion = 1.0f, \
	.farDetail = 4.0f, \
	.bodyCount = 5.0f, \
	.bodySize = 1.6f, \
	.bodyStretch = 2.0f, \
	.bodyBreakup = 0.4f, \
	.gasStrength = 0.08f, \
	.gasLight = 0.15f, \
	.gasExtent = 1.4f, \
	.gasRadius = 1.5f, \
	.gasContrast = 3.0f, \
	.gasBack = 0.9f, \
	.lightBack = 1.3f, \
	.bloomStrength = 0.6f, \
	.bloomLevels = 5.0f, \
	.skyExposure = 1.3f, \
	.skyContrast = 1.3f, \
	.filament = 1.0f, \
	.filReach = 2.8f, \
	.filStretch = 3.5f, \
	.filScale = 3.0f, \
	.filBend = 1.0f, \
	.edgeGlow = 0.3f, \
	.edgeWidth = 0.12f, \
	.bodyFill = 1.0f, \
	.densityCurve = 1.0f, \
	.filCover = 0.45f, \
	.filTaper = 0.5f, \
}

nebulaParams g_nebula = NEBULA_DEFAULTS;
int g_nebulaEnabled = NEBULA_ENABLE_DEFAULT;
char g_nebulaSkyName[64] = NEBULA_SKY_NAME;
int g_nebulaBakeSize = NEBULA_SKY_FACE_SIZE;

nebulaParams nebulaDefaults(void)
{
	nebulaParams p = NEBULA_DEFAULTS;
	return p;
}

/* Texture units (the passes have their own programs). */
#define UNIT_NOISE    0
#define UNIT_PRESENCE 1
#define UNIT_DEPTH    2
#define UNIT_FINER    1
#define UNIT_SHAPE    3
#define UNIT_DETAIL   4
#define UNIT_HDR      0
/* Far-layer noise textures (nebula_noise.frag): texels per side. */
#define SHAPE_SIZE    128
#define DETAIL_SIZE   32
/* Value-noise lattice (cloudnoise.glsl): texels per side, repeating. */
#define NOISE_SIZE 256
/* Presence grid mips: 64 -> 8 texels; the coarse walk reads the last. */
#define GRID_LEVELS 4

static GLuint skyProgram, volProgram, gridProgram, noiseProgram, postProgram;
static GLuint shapeTex, detailTex;
static unsigned int shapeSeed;
static int shapeReady;
static GLuint emptyVAO, fbo, noiseTex, gridTex, skyCube;
static int skyCubeSize;
static unsigned int noiseSeed;
static int initDone;

static GLuint sceneDepthTex;
static int sceneDepthSamples;
static float sceneNear;

/* The near-layer settings the grid was built from. */
typedef struct GridKey {
	unsigned int seed;
	float wavelength, coverage, density;
} GridKey;
static GridKey gridBuilt;
static int gridReady;

static int rebakeRequested = -1;   /* -1 none, 0 nebula, 1 axis test */
static int saveRequested;
static unsigned int frameCount;

/* Command line. */
static char bakeName[64];
static char bakeParams[64];
static int bakeSeedSet;
static unsigned int bakeSeed;
static int bakeSize = NEBULA_SKY_FACE_SIZE;
static int bakeAxes;

/* splitmix64's finaliser (as Clouds.c). */
static unsigned long long mix64(unsigned long long z)
{
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* ---- Command line -------------------------------------------------------- */

void Nebula_ParseArgs(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bake-sky") == 0 && i + 1 < argc)
			strncpy(bakeName, argv[++i], sizeof bakeName - 1);
		else if (strcmp(argv[i], "--sky-size") == 0 && i + 1 < argc)
			bakeSize = atoi(argv[++i]);
		else if (strcmp(argv[i], "--sky-params") == 0 && i + 1 < argc)
			strncpy(bakeParams, argv[++i], sizeof bakeParams - 1);
		else if (strcmp(argv[i], "--sky-seed") == 0 && i + 1 < argc) {
			bakeSeed = (unsigned int)strtoul(argv[++i], NULL, 10);
			bakeSeedSet = 1;
		}
		else if (strcmp(argv[i], "--sky-axes") == 0)
			bakeAxes = 1;
		else if (strcmp(argv[i], "--nebula") == 0)
			g_nebulaEnabled = 1;
		else if (strcmp(argv[i], "--sky") == 0 && i + 1 < argc)
			strncpy(g_nebulaSkyName, argv[++i], sizeof g_nebulaSkyName - 1);
	}
	if (bakeSize < 16)
		bakeSize = 16;
}

int Nebula_BakeRequested(void)
{
	return bakeName[0] != '\0';
}

/* ---- Lattice --------------------------------------------------------------- */

/* Value-noise lattice from the seed, as Clouds.c builds its own. R: the
   lattice; G: R at (x + 37, y + 17), the next z slice (cloudnoise.glsl). */
static void buildNoise(unsigned int seed)
{
	int n = NOISE_SIZE;
	unsigned char *r = malloc((size_t)n * n), *rg = malloc((size_t)n * n * 2);
	unsigned long long h = 0x6e6562756c61ULL ^ ((unsigned long long)seed << 32);
	for (int i = 0; i < n * n; i++) {
		h = mix64(h + 0x9e3779b97f4a7c15ULL);
		r[i] = (unsigned char)(h >> 56);
	}
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			rg[(y * n + x) * 2 + 0] = r[y * n + x];
			rg[(y * n + x) * 2 + 1] = r[((y + 17) % n) * n + (x + 37) % n];
		}
	if (!noiseTex)
		glGenTextures(1, &noiseTex);
	glBindTexture(GL_TEXTURE_2D, noiseTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, n, n, 0, GL_RG, GL_UNSIGNED_BYTE, rg);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(r);
	free(rg);
	noiseSeed = seed;
}

/* ---- Shared uniforms (nebula.glsl) ------------------------------------- */

#define MAX_BODIES 8   /* NEBULA_MAX_BODIES / NEBULA_MAX_LIGHTS, nebula.glsl */

static int clampCount(float f)
{
	int n = (int)(f + 0.5f);
	return n < 0 ? 0 : (n > MAX_BODIES ? MAX_BODIES : n);
}

/* Uniform doubles in [0, 1) from a splitmix stream. */
static double nextUnit(unsigned long long *h)
{
	*h = mix64(*h + 0x9e3779b97f4a7c15ULL);
	return (double)(*h >> 11) * (1.0 / 9007199254740992.0);
}

static void normalize3(double v[3])
{
	double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (int k = 0; k < 3; k++)
		v[k] /= l;
}

/* The far layer's composition from the seed. Bodies: directions pulled
   toward the galaxy band, centres mid-shell (clear of its faded edges),
   stretched ellipsoids at random orientations, sizes +-35 % around
   bodySize. Lights: in the first lightCount bodies, on the far side of
   the centre as seen from the sun (the body is backlit: rims), off-centre
   by a random part of the short axis; colours alternate colB / colA at
   full brightness, intensity +-40 %. */
static void setLightUniforms(GLuint s)
{
	const nebulaParams *p = &g_nebula;
	int nBodies = clampCount(p->bodyCount);
	int nLights = clampCount(p->lightCount);
	if (nLights > nBodies)
		nLights = nBodies;
	float center[MAX_BODIES * 3], inv[MAX_BODIES * 9];
	float pos[MAX_BODIES * 3], col[MAX_BODIES * 3];
	double a = p->bandTilt * M_PI / 180.0;
	double axis[3] = { 0.0, cos(a), sin(a) };
	double farEnd = NEBULA_FAR_END / NEBULA_FAR_START;
	unsigned long long h = mix64(0x11947ULL + p->seed);
	memset(pos, 0, sizeof pos);
	memset(col, 0, sizeof col);
	for (int i = 0; i < MAX_BODIES; i++) {
		/* Direction: uniform, then toward the band plane. */
		double z = 2.0 * nextUnit(&h) - 1.0, phi = 2.0 * M_PI * nextUnit(&h), s1 = sqrt(1.0 - z * z);
		double d[3] = { s1 * cos(phi), s1 * sin(phi), z };
		double y = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2];
		for (int k = 0; k < 3; k++)
			d[k] -= axis[k] * y * 0.8 * p->band;
		normalize3(d);
		double rad = 1.35 + nextUnit(&h) * (0.45 * (farEnd - 1.0));

		/* Orientation: a random orthonormal frame (long axis u). */
		double u[3] = { 2.0 * nextUnit(&h) - 1.0, 2.0 * nextUnit(&h) - 1.0, 2.0 * nextUnit(&h) - 1.0 };
		normalize3(u);
		double t[3] = { -u[1], u[0], 0.0 };
		if (fabs(u[2]) > 0.9) { t[0] = 0.0; t[1] = -u[2]; t[2] = u[1]; }
		normalize3(t);
		double b[3] = { u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0] };
		double size = p->bodySize * (0.65 + 0.7 * nextUnit(&h));
		double stretch = p->bodyStretch > 1.0f ? p->bodyStretch : 1.0;
		double ax[3] = { size, size / stretch, size / sqrt(stretch) };
		/* inv: rows are the axes / half-length (offset -> unit sphere).
		   Column-major upload: element (row r, col c) at c * 3 + r. */
		const double *rows[3] = { u, t, b };
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				inv[i * 9 + c * 3 + r] = (float)(rows[r][c] / ax[r]);
		for (int k = 0; k < 3; k++)
			center[i * 3 + k] = (float)(d[k] * rad);

		/* The body's light: behind its centre along the line from the sun,
		   lightBack times the body's depth on that line (the ellipsoid's
		   support: > 1 puts it behind the dust, so the body is backlit),
		   off axis by up to a third of the short axis. */
		double off[3] = { 2.0 * nextUnit(&h) - 1.0, 2.0 * nextUnit(&h) - 1.0, 2.0 * nextUnit(&h) - 1.0 };
		double depth = 0.0;
		for (int r = 0; r < 3; r++) {
			double dr = (rows[r][0] * d[0] + rows[r][1] * d[1] + rows[r][2] * d[2]) * ax[r];
			depth += dr * dr;
		}
		double back = p->lightBack * sqrt(depth), side = 0.33 * ax[1];
		double gainR = nextUnit(&h);
		if (i < nLights) {
			const float *tint = (i & 1) ? p->colA : p->colB;
			float m = fmaxf(tint[0], fmaxf(tint[1], tint[2]));
			float gain = p->lightIntensity * (float)(0.6 + 0.8 * gainR) / (m > 1e-3f ? m : 1.0f);
			for (int k = 0; k < 3; k++) {
				pos[i * 3 + k] = (float)(d[k] * (rad + back) + off[k] * side);
				col[i * 3 + k] = tint[k] * gain;
			}
		}
	}
	glUniform1i(glGetUniformLocation(s, "nebulaBodyCount"), nBodies);
	glUniform3fv(glGetUniformLocation(s, "nebulaBodyCenter"), MAX_BODIES, center);
	glUniformMatrix3fv(glGetUniformLocation(s, "nebulaBodyInv"), MAX_BODIES, GL_FALSE, inv);
	glUniform1f(glGetUniformLocation(s, "nebulaBodyBreakup"), p->bodyBreakup);
	glUniform1f(glGetUniformLocation(s, "nebulaFilament"), p->filament);
	glUniform1f(glGetUniformLocation(s, "nebulaFilReach"), p->filReach);
	glUniform1f(glGetUniformLocation(s, "nebulaFilStretch"), p->filStretch);
	glUniform1f(glGetUniformLocation(s, "nebulaFilScale"), p->filScale);
	glUniform1f(glGetUniformLocation(s, "nebulaFilBend"), p->filBend);
	glUniform1f(glGetUniformLocation(s, "nebulaBodyFill"), p->bodyFill);
	glUniform1f(glGetUniformLocation(s, "nebulaFilCover"), p->filCover);
	glUniform1f(glGetUniformLocation(s, "nebulaFilTaper"), p->filTaper);
	glUniform1i(glGetUniformLocation(s, "nebulaLightCount"), nLights);
	glUniform3fv(glGetUniformLocation(s, "nebulaLightPos"), MAX_BODIES, pos);
	glUniform3fv(glGetUniformLocation(s, "nebulaLightCol"), MAX_BODIES, col);
	glUniform1f(glGetUniformLocation(s, "nebulaLightRadius"), p->lightRadius);
	glUniform1f(glGetUniformLocation(s, "nebulaLightClump"), p->lightClump);
}

static float clampWavelength(float w)
{
	return w > (float)NEBULA_MIN_WAVELENGTH ? w : (float)NEBULA_MIN_WAVELENGTH;
}

static void setFieldUniforms(GLuint s)
{
	const nebulaParams *p = &g_nebula;
	if (noiseSeed != p->seed)
		buildNoise(p->seed);
	glActiveTexture(GL_TEXTURE0 + UNIT_NOISE);
	glBindTexture(GL_TEXTURE_2D, noiseTex);
	glUniform1i(glGetUniformLocation(s, "cloudNoiseTex"), UNIT_NOISE);
	glUniform1f(glGetUniformLocation(s, "cloudNoiseSize"), (float)NOISE_SIZE);

	/* A lattice offset from the seed too (lattice units, in the repeat). */
	unsigned long long h = mix64(0xa5a5ULL + p->seed);
	float off[3];
	for (int k = 0; k < 3; k++) {
		h = mix64(h + 0x9e3779b97f4a7c15ULL);
		off[k] = (float)((h >> 40) % 4096u) * (1.0f / 16.0f);
	}
	glUniform3fv(glGetUniformLocation(s, "nebulaSeed"), 1, off);
	glUniform3fv(glGetUniformLocation(s, "nebulaColA"), 1, p->colA);
	glUniform3fv(glGetUniformLocation(s, "nebulaColB"), 1, p->colB);
	glUniform1f(glGetUniformLocation(s, "nebulaTurbulence"), p->turbulence);
	glUniform1f(glGetUniformLocation(s, "nebulaContrast"), p->contrast);
	glUniform1f(glGetUniformLocation(s, "nebulaEmission"), p->emission);
	glUniform1f(glGetUniformLocation(s, "nebulaDark"), p->dark);
	glUniform4f(glGetUniformLocation(s, "nebulaNear"), clampWavelength(p->nearWavelength),
	            p->nearCoverage, p->nearSigma, p->nearDensity);
	glUniform4f(glGetUniformLocation(s, "nebulaFar"), clampWavelength(p->farWavelength),
	            p->farCoverage, p->farSigma, p->farDensity);
	glUniform1f(glGetUniformLocation(s, "nebulaFarEnd"), (float)(NEBULA_FAR_END / NEBULA_FAR_START));
	/* The band plane: the ecliptic (world XZ, normal +Y) tilted about X. */
	double a = p->bandTilt * M_PI / 180.0;
	glUniform3f(glGetUniformLocation(s, "nebulaBandAxis"), 0.0f, (float)cos(a), (float)sin(a));
	glUniform1f(glGetUniformLocation(s, "nebulaBand"), p->band);
	setLightUniforms(s);
}

/* ---- GL state for the offscreen passes ------------------------------------ */

typedef struct SavedState {
	GLint viewport[4], fbo;
	GLboolean scissor, depthTest, blend, cull;
} SavedState;

static SavedState beginOffscreen(void)
{
	SavedState s;
	glGetIntegerv(GL_VIEWPORT, s.viewport);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
	s.scissor = glIsEnabled(GL_SCISSOR_TEST);
	s.depthTest = glIsEnabled(GL_DEPTH_TEST);
	s.blend = glIsEnabled(GL_BLEND);
	s.cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glBindVertexArray(emptyVAO);
	return s;
}

static void endOffscreen(const SavedState *s)
{
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
	glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	glDisable(GL_SCISSOR_TEST);
	if (s->scissor) glEnable(GL_SCISSOR_TEST);
	if (s->depthTest) glEnable(GL_DEPTH_TEST);
	if (s->blend) glEnable(GL_BLEND);
	if (s->cull) glEnable(GL_CULL_FACE);
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);
}

/* ---- Presence grid ------------------------------------------------------- */

static GridKey currentGridKey(void)
{
	GridKey k;
	memset(&k, 0, sizeof k);
	k.seed = g_nebula.seed;
	k.wavelength = clampWavelength(g_nebula.nearWavelength);
	k.coverage = g_nebula.nearCoverage;
	k.density = g_nebula.nearDensity;
	return k;
}

/* The near layer's density bound over n in [-1, 1]^3, then max-reduced
   mips (nebula_grid.frag), a slice per draw. */
static void buildGrid(void)
{
	SavedState st = beginOffscreen();
	glUseProgram(gridProgram);
	setFieldUniforms(gridProgram);
	glUniform1i(glGetUniformLocation(gridProgram, "finer"), UNIT_FINER);

	for (int level = 0; level < GRID_LEVELS; level++) {
		int size = NEBULA_GRID_SIZE >> level;
		glUniform1i(glGetUniformLocation(gridProgram, "reduce"), level > 0);
		glUniform1f(glGetUniformLocation(gridProgram, "gridSize"), (float)size);
		/* Only the level below may be sampled: no feedback with the
		   level being written. */
		glActiveTexture(GL_TEXTURE0 + UNIT_FINER);
		glBindTexture(GL_TEXTURE_3D, gridTex);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, level > 0 ? level - 1 : 0);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, level > 0 ? level - 1 : 0);
		if (level == 0)
			glBindTexture(GL_TEXTURE_3D, 0);
		glViewport(0, 0, size, size);
		for (int z = 0; z < size; z++) {
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gridTex, level, z);
			glUniform1i(glGetUniformLocation(gridProgram, "slice"), z);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	}
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
	glBindTexture(GL_TEXTURE_3D, gridTex);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, GRID_LEVELS - 1);
	glBindTexture(GL_TEXTURE_3D, 0);
	endOffscreen(&st);

	gridBuilt = currentGridKey();
	gridReady = 1;
}

/* ---- Init ------------------------------------------------------------------ */

void Nebula_Init(void)
{
	createShader(&skyProgram, "shaders/nebula.vert", "shaders/nebula_sky.frag");
	createShader(&volProgram, "shaders/nebula.vert", "shaders/nebula_vol.frag");
	createShader(&gridProgram, "shaders/nebula.vert", "shaders/nebula_grid.frag");
	createShader(&noiseProgram, "shaders/nebula.vert", "shaders/nebula_noise.frag");
	createShader(&postProgram, "shaders/nebula.vert", "shaders/nebula_post.frag");
	glGenVertexArrays(1, &emptyVAO);
	glGenFramebuffers(1, &fbo);
	buildNoise(g_nebula.seed);

	glGenTextures(1, &gridTex);
	glBindTexture(GL_TEXTURE_3D, gridTex);
	for (int level = 0; level < GRID_LEVELS; level++) {
		int size = NEBULA_GRID_SIZE >> level;
		glTexImage3D(GL_TEXTURE_3D, level, GL_R8, size, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
	}
	/* Explicit lods only (textureLod): nearest level, trilinear within it. */
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, GRID_LEVELS - 1);
	glBindTexture(GL_TEXTURE_3D, 0);
	initDone = 1;
	/* The grid is built by Nebula_Update (after a loaded sky's settings). */
}

void Nebula_SetSceneDepth(GLuint depthTex, int samples, float nearPlane)
{
	sceneDepthTex = depthTex;
	sceneDepthSamples = samples;
	sceneNear = nearPlane;
}

/* ---- Far-layer noise textures ------------------------------------------- */

static GLuint newNoise3D(int size)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_3D, tex);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, size, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glBindTexture(GL_TEXTURE_3D, 0);
	return tex;
}

/* One texture, a slice per draw, then mips (averaging is right here). */
static void fillNoise3D(GLuint tex, int size, int kind)
{
	glUniform1i(glGetUniformLocation(noiseProgram, "kind"), kind);
	glUniform1f(glGetUniformLocation(noiseProgram, "size"), (float)size);
	glViewport(0, 0, size, size);
	for (int z = 0; z < size; z++) {
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, z);
		glUniform1i(glGetUniformLocation(noiseProgram, "slice"), z);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
	glBindTexture(GL_TEXTURE_3D, tex);
	glGenerateMipmap(GL_TEXTURE_3D);
	glBindTexture(GL_TEXTURE_3D, 0);
}

/* The shape and detail textures for the current seed, built on first use
   (only the sky bake needs them). */
static void ensureShapeTextures(void)
{
	if (shapeReady && shapeSeed == g_nebula.seed)
		return;
	if (!shapeTex) {
		shapeTex = newNoise3D(SHAPE_SIZE);
		detailTex = newNoise3D(DETAIL_SIZE);
	}
	SavedState st = beginOffscreen();
	glUseProgram(noiseProgram);
	glUniform1ui(glGetUniformLocation(noiseProgram, "seed"), g_nebula.seed);
	fillNoise3D(shapeTex, SHAPE_SIZE, 0);
	fillNoise3D(detailTex, DETAIL_SIZE, 1);
	endOffscreen(&st);
	shapeSeed = g_nebula.seed;
	shapeReady = 1;
}

/* ---- Sky bake ---------------------------------------------------------- */

static GLuint newSkyCube(int size)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	for (int f = 0; f < 6; f++)
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	return tex;
}

/* The far layer and stars into an RGBA16F cubemap (display values), each
   face in strips of NEBULA_BAKE_STRIP_ROWS rows with a glFinish between,
   so no draw nears the Windows GPU timeout. pump: also poll window events
   (the --bake-sky run has no render loop to do it). */
static void bakeSky(GLuint cube, int size, int axisDebug, int pump)
{
	ensureShapeTextures();
	/* The march writes linear radiance here; the glow pass reads it. */
	GLuint hdrCube = newSkyCube(size);
	SavedState st = beginOffscreen();
	glUseProgram(skyProgram);
	setFieldUniforms(skyProgram);
	glUniform1f(glGetUniformLocation(skyProgram, "faceSize"), (float)size);
	glUniform1i(glGetUniformLocation(skyProgram, "steps"), NEBULA_BAKE_STEPS);
	glUniform1f(glGetUniformLocation(skyProgram, "starDensity"), g_nebula.starDensity);
	glUniform1f(glGetUniformLocation(skyProgram, "starBrightness"), g_nebula.starBrightness);
	glUniform1i(glGetUniformLocation(skyProgram, "axisDebug"), axisDebug);
	glUniform1f(glGetUniformLocation(skyProgram, "dustAlbedo"), g_nebula.dustAlbedo);
	glUniform3f(glGetUniformLocation(skyProgram, "dustAmbient"), g_nebula.colA[0] * g_nebula.ambient,
	            g_nebula.colA[1] * g_nebula.ambient, g_nebula.colA[2] * g_nebula.ambient);
	glUniform1f(glGetUniformLocation(skyProgram, "phaseG"), g_nebula.phaseG);
	glUniform1f(glGetUniformLocation(skyProgram, "powder"), g_nebula.powder);
	glUniform1f(glGetUniformLocation(skyProgram, "coreSize"), g_nebula.coreSize);
	glActiveTexture(GL_TEXTURE0 + UNIT_SHAPE);
	glBindTexture(GL_TEXTURE_3D, shapeTex);
	glActiveTexture(GL_TEXTURE0 + UNIT_DETAIL);
	glBindTexture(GL_TEXTURE_3D, detailTex);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(skyProgram, "nebulaShapeTex"), UNIT_SHAPE);
	glUniform1i(glGetUniformLocation(skyProgram, "nebulaDetailTex"), UNIT_DETAIL);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaShapeSize"), (float)SHAPE_SIZE);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaDetailSize"), (float)DETAIL_SIZE);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaDetailFreq"), g_nebula.farDetail);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaErosion"), g_nebula.farErosion);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaDensityCurve"), g_nebula.densityCurve > 0.2f ? g_nebula.densityCurve : 0.2f);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasStrength"), g_nebula.gasStrength);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasLight"), g_nebula.gasLight);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasExtent"), g_nebula.gasExtent > 0.1f ? g_nebula.gasExtent : 0.1f);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasRadius"), g_nebula.gasRadius > 0.01f ? g_nebula.gasRadius : 0.01f);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasContrast"), g_nebula.gasContrast);
	glUniform1f(glGetUniformLocation(skyProgram, "nebulaGasBack"), g_nebula.gasBack);
	glUniform1f(glGetUniformLocation(skyProgram, "edgeGlow"), g_nebula.edgeGlow);
	glUniform1f(glGetUniformLocation(skyProgram, "edgeWidth"), g_nebula.edgeWidth > 0.01f ? g_nebula.edgeWidth : 0.01f);
	glViewport(0, 0, size, size);
	glEnable(GL_SCISSOR_TEST);
	for (int f = 0; f < 6; f++) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, hdrCube, 0);
		glUniform1i(glGetUniformLocation(skyProgram, "face"), f);
		for (int y = 0; y < size; y += NEBULA_BAKE_STRIP_ROWS) {
			glScissor(0, y, size, NEBULA_BAKE_STRIP_ROWS);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glFinish();
			if (pump)
				glfwPollEvents();
		}
		if (pump)
			fprintf(stderr, "[nebula] face %d/6\n", f + 1);
	}
	glDisable(GL_SCISSOR_TEST);

	/* Glow pass: mips of the HDR cube (box-filtered, seamless), then
	   bloom + tone map + contrast into the display-valued sky. Seamless
	   cube filtering stays on: the planet's baked maps are cube maps too
	   (TerrainGen.c) and need it. */
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_HDR);
	glBindTexture(GL_TEXTURE_CUBE_MAP, hdrCube);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
	glUseProgram(postProgram);
	glUniform1i(glGetUniformLocation(postProgram, "hdr"), UNIT_HDR);
	glUniform1f(glGetUniformLocation(postProgram, "faceSize"), (float)size);
	glUniform1i(glGetUniformLocation(postProgram, "axisDebug"), axisDebug);
	glUniform1f(glGetUniformLocation(postProgram, "bloomStrength"), g_nebula.bloomStrength);
	int levels = (int)(g_nebula.bloomLevels + 0.5f);
	glUniform1i(glGetUniformLocation(postProgram, "bloomLevels"), levels < 0 ? 0 : (levels > 9 ? 9 : levels));
	glUniform1f(glGetUniformLocation(postProgram, "exposure"), g_nebula.skyExposure);
	glUniform1f(glGetUniformLocation(postProgram, "contrast"), g_nebula.skyContrast > 0.1f ? g_nebula.skyContrast : 0.1f);
	for (int f = 0; f < 6; f++) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, cube, 0);
		glUniform1i(glGetUniformLocation(postProgram, "face"), f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glFinish();
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	endOffscreen(&st);
	glDeleteTextures(1, &hdrCube);
}

static void skyDir(char *out, size_t n, const char *name)
{
	snprintf(out, n, "../assets/textures/nebula_%s", name);
}

/* ---- Settings file -------------------------------------------------------- */

typedef struct Field {
	const char *key;
	size_t offset;
	int count;
} Field;
#define FIELD(name, n) { #name, offsetof(nebulaParams, name), n }
static const Field fields[] = {
	FIELD(nearWavelength, 1), FIELD(nearCoverage, 1), FIELD(nearDensity, 1), FIELD(nearSigma, 1),
	FIELD(farWavelength, 1), FIELD(farCoverage, 1), FIELD(farDensity, 1), FIELD(farSigma, 1),
	FIELD(turbulence, 1), FIELD(contrast, 1), FIELD(emission, 1), FIELD(dark, 1),
	FIELD(colA, 3), FIELD(colB, 3), FIELD(band, 1), FIELD(bandTilt, 1),
	FIELD(starDensity, 1), FIELD(starBrightness, 1), FIELD(scatter, 1),
	FIELD(lightCount, 1), FIELD(lightIntensity, 1), FIELD(lightRadius, 1), FIELD(lightClump, 1),
	FIELD(dustAlbedo, 1), FIELD(ambient, 1), FIELD(phaseG, 1), FIELD(powder, 1), FIELD(coreSize, 1),
	FIELD(farErosion, 1), FIELD(farDetail, 1),
	FIELD(bodyCount, 1), FIELD(bodySize, 1), FIELD(bodyStretch, 1), FIELD(bodyBreakup, 1),
	FIELD(gasStrength, 1), FIELD(gasLight, 1), FIELD(gasExtent, 1), FIELD(gasRadius, 1), FIELD(gasContrast, 1), FIELD(gasBack, 1),
	FIELD(lightBack, 1), FIELD(bloomStrength, 1), FIELD(bloomLevels, 1), FIELD(skyExposure, 1), FIELD(skyContrast, 1),
	FIELD(filament, 1), FIELD(filReach, 1), FIELD(filStretch, 1), FIELD(filScale, 1), FIELD(filBend, 1),
	FIELD(edgeGlow, 1), FIELD(edgeWidth, 1), FIELD(bodyFill, 1), FIELD(densityCurve, 1),
	FIELD(filCover, 1), FIELD(filTaper, 1),
};
#define FIELD_COUNT (int)(sizeof fields / sizeof fields[0])

static int writeParams(const char *path, int faceSize)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	fprintf(f, "# Nebula sky settings (Nebula.c); loaded with the faces.\n");
	fprintf(f, "# radiusM %.6g farStart %.6g farEnd %.6g faceSize %d\n",
	        NEBULA_RADIUS_M, NEBULA_FAR_START, NEBULA_FAR_END, faceSize);
	fprintf(f, "seed %u\n", g_nebula.seed);
	for (int i = 0; i < FIELD_COUNT; i++) {
		const float *v = (const float *)((const char *)&g_nebula + fields[i].offset);
		fprintf(f, "%s", fields[i].key);
		for (int k = 0; k < fields[i].count; k++)
			fprintf(f, " %.6g", v[k]);
		fprintf(f, "\n");
	}
	fclose(f);
	return 1;
}

int Nebula_LoadParams(const char *name)
{
	char dir[256], path[320];
	skyDir(dir, sizeof dir, name);
	snprintf(path, sizeof path, "%s.txt", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char line[256], key[64];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || sscanf(line, "%63s", key) != 1)
			continue;
		const char *vals = line + strlen(key);
		if (strcmp(key, "seed") == 0) {
			sscanf(vals, "%u", &g_nebula.seed);
			continue;
		}
		for (int i = 0; i < FIELD_COUNT; i++) {
			if (strcmp(key, fields[i].key) != 0)
				continue;
			float *v = (float *)((char *)&g_nebula + fields[i].offset);
			if (fields[i].count == 1)
				sscanf(vals, "%f", &v[0]);
			else
				sscanf(vals, "%f %f %f", &v[0], &v[1], &v[2]);
		}
	}
	fclose(f);
	return 1;
}

/* ---- PNG faces ------------------------------------------------------------- */

static unsigned int xorshift(unsigned int *s)
{
	unsigned int x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

/* Six 8-bit faces from the RGBA16F cube, with triangular (+-1 LSB) dither:
   dark smooth gas bands at 8 bits otherwise. File for face f = entry f of
   Skybox.c's list (loadCubemap uploads faces[i] to POSITIVE_X + i). Rows:
   glGetTexImage returns row 0 (t = 0) first and loadCubemap uploads the
   PNG's top row as row 0, so no flip on write (Bench.c turns it on for its
   window captures, and sets it again before each). */
static int writeSky(GLuint cube, int size, const char *name)
{
	static const char *faceNames[6] = { "right", "left", "top", "bottom", "back", "front" };
	char dir[256], path[320];
	skyDir(dir, sizeof dir, name);
	_mkdir("../assets/textures");
	_mkdir(dir);

	float *px = malloc((size_t)size * size * 4 * sizeof(float));
	unsigned char *out = malloc((size_t)size * size * 3);
	if (!px || !out) {
		free(px);
		free(out);
		return 0;
	}
	unsigned int rng = 0x9e3779b9u ^ g_nebula.seed;
	int ok = 1;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
	stbi_flip_vertically_on_write(0);
	for (int f = 0; f < 6 && ok; f++) {
		glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, GL_FLOAT, px);
		for (size_t i = 0; i < (size_t)size * size; i++)
			for (int c = 0; c < 3; c++) {
				float r1 = (xorshift(&rng) >> 8) * (1.0f / 16777216.0f);
				float r2 = (xorshift(&rng) >> 8) * (1.0f / 16777216.0f);
				float v = px[i * 4 + c] * 255.0f + (r1 - r2) + 0.5f;
				out[i * 3 + c] = (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
			}
		snprintf(path, sizeof path, "%s/%s.png", dir, faceNames[f]);
		ok = stbi_write_png(path, size, size, 3, out, size * 3);
		fprintf(stderr, "[nebula] %s %s\n", ok ? "wrote" : "FAILED", path);
	}
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	free(px);
	free(out);

	snprintf(path, sizeof path, "%s.txt", dir);
	if (ok && !writeParams(path, size))
		ok = 0;
	return ok;
}

int Nebula_RunBake(void)
{
	/* Settings: WorldConfig's defaults, or another set's file (tuning:
	   edit nebula_<set>.txt, bake with --sky-params <set>). */
	if (bakeParams[0] && !Nebula_LoadParams(bakeParams)) {
		fprintf(stderr, "[nebula] no settings file for '%s'\n", bakeParams);
		return 1;
	}
	/* --sky-seed overrides the defaults' or the settings file's seed (a
	   new layout with the same look); the .txt written records it. */
	if (bakeSeedSet)
		g_nebula.seed = bakeSeed;
	Nebula_Init();
	fprintf(stderr, "[nebula] baking '%s' at %d^2%s\n", bakeName, bakeSize, bakeAxes ? " (axis test)" : "");
	GLuint cube = newSkyCube(bakeSize);
	bakeSky(cube, bakeSize, bakeAxes, 1);
	int ok = writeSky(cube, bakeSize, bakeName);
	glDeleteTextures(1, &cube);
	return ok ? 0 : 1;
}

/* ---- Per frame ------------------------------------------------------------- */

void Nebula_RequestRebake(int axisDebug)
{
	rebakeRequested = axisDebug ? 1 : 0;
}

void Nebula_RequestSave(void)
{
	saveRequested = 1;
}

void Nebula_Update(void)
{
	if (!initDone)
		return;
	frameCount++;
	GridKey k = currentGridKey();
	if (!gridReady || memcmp(&k, &gridBuilt, sizeof k) != 0)
		buildGrid();

	if (saveRequested && !skyCube)
		rebakeRequested = 0;   /* nothing baked yet: bake what is set */
	if (rebakeRequested >= 0) {
		int size = g_nebulaBakeSize < 16 ? 16 : g_nebulaBakeSize;
		if (skyCube && skyCubeSize != size) {
			glDeleteTextures(1, &skyCube);
			skyCube = 0;
		}
		if (!skyCube) {
			skyCube = newSkyCube(size);
			skyCubeSize = size;
		}
		bakeSky(skyCube, size, rebakeRequested, 0);
		rebakeRequested = -1;
	}
	if (saveRequested) {
		const char *name = g_nebulaSkyName[0] ? g_nebulaSkyName : "custom";
		writeSky(skyCube, skyCubeSize, name);
		saveRequested = 0;
	}
}

GLuint Nebula_SkyTexture(GLuint fallback)
{
	return skyCube ? skyCube : fallback;
}

/* ---- Near layer -------------------------------------------------------- */

/* Whether the unit sphere can be on screen: always from inside; from
   outside, if its angular disc reaches the view cone (the frustum's
   half-diagonal). fwd: camera forward, world axes. */
static int sphereVisible(dvec3 c, dvec3 fwd, double aspect)
{
	double d = dlengthvec3(c);
	if (d <= 1.0)
		return 1;
	double cosA = -(c.x * fwd.x + c.y * fwd.y + c.z * fwd.z) / d;
	double ang = acos(fmax(-1.0, fmin(1.0, cosA)));
	double halfY = 0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0;
	double halfDiag = atan(tan(halfY) * sqrt(1.0 + aspect * aspect));
	return ang <= halfDiag + asin(1.0 / d);
}

void Nebula_Draw(mat4 proj, mat4 view, dvec3 cam)
{
	if (!g_nebulaEnabled || !gridReady)
		return;
	double R = NEBULA_RADIUS_M;
	dvec3 rel = dsubvec3(cam, g_bodies[BODY_SUN].position);
	dvec3 camN = dscalarvec3(rel, 1.0 / R);
	/* Forward: minus the view matrix's third row (Atmosphere.c). */
	dvec3 fwd = { -view.m[2][0], -view.m[2][1], -view.m[2][2] };
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	if (!sphereVisible(camN, fwd, (double)viewport[2] / (double)viewport[3]))
		return;

	GLuint s = volProgram;
	glUseProgram(s);
	setFieldUniforms(s);
	glUniformMatrix4fv(glGetUniformLocation(s, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniform2f(glGetUniformLocation(s, "viewportSize"), (float)viewport[2], (float)viewport[3]);
	/* View -> world: column j is camera axis j (row j of the view matrix). */
	float v2w[9];
	for (int j = 0; j < 3; j++)
		for (int k = 0; k < 3; k++)
			v2w[j * 3 + k] = view.m[j][k];
	glUniformMatrix3fv(glGetUniformLocation(s, "viewToWorld"), 1, GL_FALSE, v2w);
	glUniform3f(glGetUniformLocation(s, "camN"), (float)camN.x, (float)camN.y, (float)camN.z);
	glUniform1f(glGetUniformLocation(s, "radiusM"), (float)R);
	glUniform1f(glGetUniformLocation(s, "pixAngle"),
	            (float)(2.0 * tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0) / viewport[3]));
	glUniform1i(glGetUniformLocation(s, "steps"), NEBULA_VOL_STEPS);
	/* Golden-ratio sequence: the jitter pattern moves every frame. */
	glUniform1f(glGetUniformLocation(s, "frameJitter"), (float)fmod(frameCount * 0.61803398875, 1.0));
	glUniform1f(glGetUniformLocation(s, "scatter"), g_nebula.scatter);
	glUniform3f(glGetUniformLocation(s, "sunColor"), 1.0f, 0.97f, 0.90f);

	glActiveTexture(GL_TEXTURE0 + UNIT_PRESENCE);
	glBindTexture(GL_TEXTURE_3D, gridTex);
	glUniform1i(glGetUniformLocation(s, "presence"), UNIT_PRESENCE);
	glUniform1f(glGetUniformLocation(s, "presenceCoarseLod"), (float)(GRID_LEVELS - 1));

	/* Scene depth, as the dome reads it (Atmosphere.c). */
	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneDepthTex);
	glUniform1i(glGetUniformLocation(s, "sceneDepth"), UNIT_DEPTH);
	glUniform1i(glGetUniformLocation(s, "sceneSamples"), sceneDepthTex ? sceneDepthSamples : 0);
	glUniform1f(glGetUniformLocation(s, "depthNear"), sceneNear);
	glUniform3f(glGetUniformLocation(s, "camForward"), (float)fwd.x, (float)fwd.y, (float)fwd.z);
	glActiveTexture(GL_TEXTURE0);

	/* Premultiplied over the scene; the march does its own occlusion
	   against the depth it reads (the bound target's own attachment, so no
	   depth test or writes). */
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	if (cull)
		glEnable(GL_CULL_FACE);

	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glActiveTexture(GL_TEXTURE0 + UNIT_PRESENCE);
	glBindTexture(GL_TEXTURE_3D, 0);
	glActiveTexture(GL_TEXTURE0);
}
