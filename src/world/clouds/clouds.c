#include "engine/gfx/shader.h"
#include "world/clouds/clouds.h"
#include "world/clouds/cloud_volume.h"
#include "world/bodies/solar_system.h"
#include "world/atmosphere/atmosphere.h"
#include "engine/platform/window.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"

/* Earth's layer (WorldConfig.h). A macro so the static initializer and
   Clouds_Defaults() share one copy. Colour: a slightly warm white. */
#define CLOUD_DEFAULTS { \
	.style = CLOUD_STYLE_CLIMATE, \
	.coverage = (float)CLOUD_EARTH_COVERAGE, \
	.climateWeight = (float)CLOUD_EARTH_CLIMATE_WEIGHT, \
	.altitude = (float)CLOUD_EARTH_ALTITUDE_M, \
	.thickness = (float)CLOUD_EARTH_THICKNESS_M, \
	.opacity = (float)CLOUD_EARTH_OPACITY, \
	.weatherScale = (float)CLOUD_EARTH_WEATHER_SCALE, \
	.sharpness = 0.6f, \
	.detail = 0.6f, \
	.bands = 8.0f, \
	.color = { 0.95f, 0.95f, 0.93f }, \
	.flowPeriod = (float)CLOUD_EARTH_FLOW_PERIOD_S, \
	.seed = 1u, \
}

static cloudParams g_clouds = CLOUD_DEFAULTS;
static int g_cloudDetailOctaves = CLOUD_DETAIL_OCTAVES;
static int g_cloudsEnabled = CLOUDS_ENABLED_DEFAULT;

cloudParams * Clouds_Params(void)
{
	return &g_clouds;
}

void Clouds_ParseArgs(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--clouds") == 0 && i + 1 < argc)
			g_cloudsEnabled = atoi(argv[++i]) != 0;
}

cloudParams Clouds_Defaults(void)
{
	cloudParams c = CLOUD_DEFAULTS;
	return c;
}

const char *Clouds_StyleName(int style)
{
	static const char *names[CLOUD_STYLE_COUNT] = { "None", "Climate", "Patchy", "Banded", "Overcast" };
	return style >= 0 && style < CLOUD_STYLE_COUNT ? names[style] : "?";
}

/* Texture units. Receivers (quad / water): 0-13 are taken. The stand-ins
   have their own program: 0-1. */
#define RECEIVER_UNIT   14
#define MAP_UNIT_NOISE  2
/* Value-noise lattice (cloudnoise.glsl): texels per side, repeating. */
#define CLOUD_NOISE_SIZE 256
/* terrain_gen.c's ocean map stores wind speed / WIND_MAX_MS. */
#define WIND_MAP_MAX_MS 20.0f

static GLuint mapProgram, cubeProgram, skyProgram;
#define MAX_DETAIL_OCTAVES 12
static GLuint mapFBO, emptyVAO, noiseTex;
static int mapW, mapH;
static float mapMaxLod;

/* A planet's two newest weather maps: tex[0] at step * CLOUD_MAP_STEP_S,
   tex[1] one step later, and the settings they were built with. serial:
   new with every build (from mapSerial), so the weather cube, made from
   these, knows when they changed. */
typedef struct CloudMaps {
	GLuint tex[2];
	long long step;
	int ready;
	unsigned int serial;
	cloudParams built;
} CloudMaps;
static CloudMaps maps[BODY_COUNT];
static unsigned int mapSerial;
static double frameTime;

/* The active planet's state is in the globals; the rest in PlanetInfo. */
static const cloudParams *paramsFor(int body)
{
	return body == SolarSystem_ActiveBody() ? &g_clouds : &SolarSystem_Info(body)->clouds;
}

static const PlanetGenParams *genFor(int body)
{
	return body == SolarSystem_ActiveBody() ? TerrainGen_GenParams() : &SolarSystem_Info(body)->gen;
}

static int wantsClouds(int body)
{
	const PlanetGenParams *g = genFor(body);
	return g_cloudsEnabled && SolarSystem_Info(body)->baked && paramsFor(body)->style != CLOUD_STYLE_NONE
	    && g->climateTex && g->oceanTex;
}

/* splitmix64's finaliser: noise lattice values and flow-cycle seeds. */
static unsigned long long mix64(unsigned long long z)
{
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

void Clouds_Init(void)
{
	Shader_Create(&mapProgram, "world/clouds/cloudmap.vert", "world/clouds/cloudmap.frag");
	Shader_Create(&cubeProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudweather.frag");
	Shader_Create(&skyProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudsky.frag");

	mapW = CLOUD_MAP_WIDTH;
	mapH = CLOUD_MAP_WIDTH / 2;
	mapMaxLod = (float)floor(log2((double)mapW));
	glGenFramebuffers(1, &mapFBO);
	/* Core profile: attribute-less draws still need a VAO bound. */
	glGenVertexArrays(1, &emptyVAO);

	/* Random lattice values for the value noise, from a fixed seed (every
	   planet offsets into it by its own seed). R: the lattice; G: R at
	   (x + 37, y + 17), the next z slice (cloudnoise.glsl). */
	int n = CLOUD_NOISE_SIZE;
	unsigned char *r = malloc((size_t)n * n), *rg = malloc((size_t)n * n * 2);
	unsigned long long h = 0x5eedc10dULL;
	for (int i = 0; i < n * n; i++) {
		h = mix64(h + 0x9e3779b97f4a7c15ULL);
		r[i] = (unsigned char)(h >> 56);
	}
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			rg[(y * n + x) * 2 + 0] = r[y * n + x];
			rg[(y * n + x) * 2 + 1] = r[((y + 17) % n) * n + (x + 37) % n];
		}
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
}

static void bindNoise(GLuint s, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, noiseTex);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(s, "cloudNoiseTex"), unit);
	glUniform1f(glGetUniformLocation(s, "cloudNoiseSize"), (float)CLOUD_NOISE_SIZE);
}

void Clouds_Drop(int body)
{
	CloudMaps *m = &maps[body];
	glDeleteTextures(2, m->tex);
	memset(m, 0, sizeof *m);
}

static GLuint newMapTexture(void)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mapW, mapH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

/* ---- Flow phases (cloudnoise.glsl) ----------------------------------------
   In double: the time is unbounded, so only the wrapped phase, the triangle
   weight and the cycle's seed reach the GPU. */
typedef struct FlowPhases {
	float tau[2], weight[2], seed[2][3];
} FlowPhases;

static FlowPhases flowAt(double t, const cloudParams *c)
{
	FlowPhases f;
	for (int i = 0; i < 2; i++) {
		double cycle = 0.0, frac = 0.5;
		if (c->flowPeriod > 0.0f) {
			double ph = t / c->flowPeriod + 0.5 * i;
			cycle = floor(ph);
			frac = ph - cycle;
			f.tau[i] = (float)(frac * c->flowPeriod);
			f.weight[i] = (float)(1.0 - fabs(2.0 * frac - 1.0));
		} else {
			f.tau[i] = 0.0f;
			f.weight[i] = i == 0 ? 1.0f : 0.0f;
		}
		/* A new noise offset per cycle and phase (lattice units). */
		unsigned long long h = mix64(((unsigned long long)(long long)cycle << 1 | (unsigned)i)
		                             ^ ((unsigned long long)c->seed << 40));
		for (int k = 0; k < 3; k++) {
			h = mix64(h + 0x9e3779b97f4a7c15ULL);
			f.seed[i][k] = (float)((h >> 40) % 4096u) * (1.0f / 16.0f);
		}
	}
	return f;
}

static void setFlowUniforms(GLuint s, const FlowPhases *f)
{
	glUniform2f(glGetUniformLocation(s, "cloudFlowTau"), f->tau[0], f->tau[1]);
	glUniform2f(glGetUniformLocation(s, "cloudFlowWeight"), f->weight[0], f->weight[1]);
	glUniform3fv(glGetUniformLocation(s, "cloudFlowSeed0"), 1, f->seed[0]);
	glUniform3fv(glGetUniformLocation(s, "cloudFlowSeed1"), 1, f->seed[1]);
}

/* ---- Weather maps ------------------------------------------------------ */

static void buildMap(int body, GLuint tex, double t)
{
	const cloudParams *c = paramsFor(body);
	const PlanetGenParams *g = genFor(body);
	double R = Bodies_Get(body)->radius;

	glBindFramebuffer(GL_FRAMEBUFFER, mapFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, mapW, mapH);
	glUseProgram(mapProgram);
	glUniform2f(glGetUniformLocation(mapProgram, "mapSize"), (float)mapW, (float)mapH);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g->climateTex);
	glUniform1i(glGetUniformLocation(mapProgram, "climateMap"), 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g->oceanTex);
	glUniform1i(glGetUniformLocation(mapProgram, "windMap"), 1);
	glUniform1f(glGetUniformLocation(mapProgram, "windMax"), WIND_MAP_MAX_MS);
	glUniform1f(glGetUniformLocation(mapProgram, "planetRadius"), (float)R);
	glUniform1f(glGetUniformLocation(mapProgram, "texelM"), (float)(2.0 * M_PI * R / mapW));
	glUniform1i(glGetUniformLocation(mapProgram, "style"), c->style);
	glUniform1f(glGetUniformLocation(mapProgram, "coverage"), c->coverage);
	glUniform1f(glGetUniformLocation(mapProgram, "climateWeight"), c->climateWeight);
	glUniform1f(glGetUniformLocation(mapProgram, "weatherScale"), c->weatherScale);
	glUniform1f(glGetUniformLocation(mapProgram, "bands"), c->bands);
	FlowPhases f = flowAt(t, c);
	setFlowUniforms(mapProgram, &f);
	bindNoise(mapProgram, MAP_UNIT_NOISE);

	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_2D, tex);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Brings body's maps up to frameTime within the frame's build budget. */
static void updateBody(int body, int *budget)
{
	CloudMaps *m = &maps[body];
	if (!wantsClouds(body)) {
		if (m->tex[0])
			Clouds_Drop(body);
		return;
	}
	const cloudParams *c = paramsFor(body);
	long long k = (long long)floor(frameTime / CLOUD_MAP_STEP_S);
	int current = m->ready && memcmp(&m->built, c, sizeof *c) == 0;
	if (current && k == m->step)
		return;
	if (current && k == m->step + 1) {
		/* The usual case: the newer map becomes the older one. */
		if (*budget < 1)
			return;
		GLuint t = m->tex[0];
		m->tex[0] = m->tex[1];
		m->tex[1] = t;
		m->step = k;
		buildMap(body, m->tex[1], (k + 1) * CLOUD_MAP_STEP_S);
		m->serial = ++mapSerial;
		*budget -= 1;
		return;
	}
	/* New settings, a first build or a time jump: both maps. */
	if (*budget < 2)
		return;
	for (int i = 0; i < 2; i++) {
		if (!m->tex[i])
			m->tex[i] = newMapTexture();
		buildMap(body, m->tex[i], (k + i) * CLOUD_MAP_STEP_S);
	}
	m->step = k;
	m->built = *c;
	m->ready = 1;
	m->serial = ++mapSerial;
	*budget -= 2;
}

void Clouds_Update(double simTime)
{
	frameTime = simTime;

	/* Map builds draw into their own target with the default state. */
	GLint viewport[4], fbo;
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);

	int budget = CLOUD_MAP_BUILDS_PER_FRAME;
	int active = SolarSystem_ActiveBody();
	updateBody(active, &budget);
	for (int i = 0; i < BODY_COUNT; i++)
		if (i != active)
			updateBody(i, &budget);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (depthTest) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
	if (cull) glEnable(GL_CULL_FACE);
}

/* ---- Uniforms shared by the bake, receivers and stand-ins ---------------- */

static float mapBlend(const CloudMaps *m)
{
	double b = (frameTime - m->step * CLOUD_MAP_STEP_S) / CLOUD_MAP_STEP_S;
	return (float)fmin(fmax(b, 0.0), 1.0);
}

static void bindMaps(GLuint s, const CloudMaps *m, int unitA, int unitB)
{
	glActiveTexture(GL_TEXTURE0 + unitA);
	glBindTexture(GL_TEXTURE_2D, m->tex[0]);
	glActiveTexture(GL_TEXTURE0 + unitB);
	glBindTexture(GL_TEXTURE_2D, m->tex[1]);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(s, "cloudMapA"), unitA);
	glUniform1i(glGetUniformLocation(s, "cloudMapB"), unitB);
	glUniform1f(glGetUniformLocation(s, "cloudMapBlend"), mapBlend(m));
	glUniform1f(glGetUniformLocation(s, "cloudMapMaxLod"), mapMaxLod);
	glUniform1f(glGetUniformLocation(s, "cloudTexelAngle"), (float)(2.0 * M_PI / mapW));
}

/* Half-width of a cloud edge in coverage-field units (the stand-ins):
   soft to hard. */
static float cloudEdge(const cloudParams *c)
{
	return 0.06f + (0.004f - 0.06f) * c->sharpness;
}

/* Radians per screen pixel at the view centre. */
static float pixelAngle(void)
{
	return (float)(2.0 * tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0) / Window_Height());
}

/* ---- The weather cube (cloudweather.frag) ---------------------------------
   The active planet's weather for the volume, over the whole body: made
   from its weather maps and their detail noise. Made whole for a new
   planet or new settings; as the maps move on in time (a new build,
   CLOUD_VOL_REBAKE_BLEND of their cross-fade) re-made a face a frame, so
   the cost is spread (a pass moves the weather only that little, so faces
   a frame apart don't show), the mips at the pass's end. */

static GLuint cubeTex, cubeFBO;
/* The next face a re-make pass makes (6: no pass). */
static int cubeFace = 6;
static int cubeBody = -1;
static unsigned int cubeSerial;
static float cubeBlend;
static int cubeOctaves;
static cloudParams cubeParams;

/* The first detail octave's wavelength: ~4 map texels at the layer. */
static double detailLambda0(int body)
{
	return 4.0 * 2.0 * M_PI * (Bodies_Get(body)->radius + paramsFor(body)->altitude) / mapW;
}

/* Detail octaves for body: g_cloudDetailOctaves, fewer where they would go
   below CLOUD_DETAIL_MIN_WAVELENGTH_M. */
static int detailOctaves(int body)
{
	int want = g_cloudDetailOctaves < 0 ? 0 : (g_cloudDetailOctaves > MAX_DETAIL_OCTAVES ? MAX_DETAIL_OCTAVES : g_cloudDetailOctaves);
	double lambda0 = detailLambda0(body);
	int octaves = want > 0 ? 1 : 0;
	while (octaves < want && lambda0 / pow(2.0, octaves) >= CLOUD_DETAIL_MIN_WAVELENGTH_M)
		octaves++;
	return octaves;
}

/* A cube texel at the centre of a face, m, on a sphere of radius r. */
static double cubeTexelM(double r)
{
	return 2.0 * r / CLOUD_CUBE_SIZE;
}

static int cubeReady(int body)
{
	return cubeTex && cubeBody == body && maps[body].ready;
}

/* Makes faces f0 .. f1 - 1 (the mips too when f1 is 6). */
static void bakeCubeFaces(int body, int f0, int f1)
{
	const cloudParams *c = paramsFor(body);
	const CloudMaps *m = &maps[body];
	int n = CLOUD_CUBE_SIZE;
	if (!cubeTex) {
		glGenTextures(1, &cubeTex);
		glBindTexture(GL_TEXTURE_CUBE_MAP, cubeTex);
		for (int f = 0; f < 6; f++)
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		glGenFramebuffers(1, &cubeFBO);
	}

	GLint fbo, vp[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
	GLboolean cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, cubeFBO);
	glViewport(0, 0, n, n);

	GLuint p = cubeProgram;
	glUseProgram(p);
	bindMaps(p, m, 0, 1);
	double rc = Bodies_Get(body)->radius + c->altitude;
	glUniform1f(glGetUniformLocation(p, "faceSize"), (float)n);
	glUniform1f(glGetUniformLocation(p, "texelM"), (float)cubeTexelM(rc));
	glUniform1f(glGetUniformLocation(p, "cloudRadius"), (float)rc);
	glUniform1f(glGetUniformLocation(p, "cloudDetail"), c->detail);
	FlowPhases fl = flowAt(frameTime, c);
	setFlowUniforms(p, &fl);
	bindNoise(p, 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_CUBE_MAP, genFor(body)->oceanTex);
	glUniform1i(glGetUniformLocation(p, "cloudWind"), 3);
	glUniform1f(glGetUniformLocation(p, "windMax"), WIND_MAP_MAX_MS);
	glUniform1f(glGetUniformLocation(p, "detailLambda0"), (float)detailLambda0(body));
	glUniform1i(glGetUniformLocation(p, "detailOctaves"), detailOctaves(body));
	glUniform1i(glGetUniformLocation(p, "cyclonesOn"), c->style == CLOUD_STYLE_CLIMATE);
	glBindVertexArray(emptyVAO);
	for (int f = f0; f < f1; f++) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, cubeTex, 0);
		glUniform1i(glGetUniformLocation(p, "face"), f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (f1 >= 6) {
		glBindTexture(GL_TEXTURE_CUBE_MAP, cubeTex);
		glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
	if (cull) glEnable(GL_CULL_FACE);
	if (scissor) glEnable(GL_SCISSOR_TEST);
}

/* Records what the cube is (being) made from. */
static void setCubeKey(int body)
{
	cubeBody = body;
	cubeSerial = maps[body].serial;
	cubeBlend = mapBlend(&maps[body]);
	cubeOctaves = detailOctaves(body);
	cubeParams = *paramsFor(body);
}

/* Brings the cube up to date with body's weather (see above). */
static void updateCube(int body)
{
	const CloudMaps *m = &maps[body];
	if (!cubeTex || cubeBody != body || cubeOctaves != detailOctaves(body)
	    || memcmp(&cubeParams, paramsFor(body), sizeof cubeParams) != 0) {
		setCubeKey(body);
		bakeCubeFaces(body, 0, 6);
		cubeFace = 6;
		return;
	}
	if (cubeFace >= 6 && (cubeSerial != m->serial || fabsf(mapBlend(m) - cubeBlend) > CLOUD_VOL_REBAKE_BLEND)) {
		setCubeKey(body);
		cubeFace = 0;
	}
	if (cubeFace < 6) {
		bakeCubeFaces(body, cubeFace, cubeFace + 1);
		cubeFace++;
	}
}

/* cloudlayer.glsl's uniforms (the receivers' shadow). */
static void setLayerUniforms(GLuint s, int body, int unit)
{
	int on = cubeReady(body);
	glUniform1f(glGetUniformLocation(s, "cloudsOn"), on ? 1.0f : 0.0f);
	if (!on)
		return;
	const cloudParams *c = paramsFor(body);
	const Body *b = Bodies_Get(body);
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cubeTex);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(s, "cloudCube"), unit);
	glUniform1f(glGetUniformLocation(s, "cloudCubeTexelM"), (float)cubeTexelM(b->radius + c->altitude));
	glUniform1f(glGetUniformLocation(s, "cloudCubeMaxLod"), (float)log2((double)CLOUD_CUBE_SIZE));
	/* World -> body-fixed: the transpose of the body's rotation (its
	   columns are the body axes in world coordinates). Column-major upload:
	   element (row i, column k) at k * 3 + i. */
	float w2b[9];
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
			w2b[k * 3 + i] = (float)b->rotation.m[k][i];
	glUniformMatrix3fv(glGetUniformLocation(s, "cloudWorldToBody"), 1, GL_FALSE, w2b);
	glUniform1f(glGetUniformLocation(s, "cloudRadius"), (float)(b->radius + c->altitude));
	glUniform1f(glGetUniformLocation(s, "cloudThickness"), c->thickness);
	glUniform1f(glGetUniformLocation(s, "cloudOpacity"), c->opacity);
	glUniform1f(glGetUniformLocation(s, "cloudPixAngle"), pixelAngle());
}

void Clouds_BindReceiver(GLuint shader, int body)
{
	setLayerUniforms(shader, body, RECEIVER_UNIT);
}

/* ---- The volume over the active planet (cloud_volume.c) -----------------
   The layer is drawn by the shared cloud volume, from the ground to orbit:
   one layer over the layer's thickness, its weather the cube above. */

static CloudVolumeScene volScene;

static dvec3 dcross(dvec3 a, dvec3 b)
{
	return (dvec3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static dvec3 dnormal(dvec3 v)
{
	return dscalarvec3(v, 1.0 / dlengthvec3(v));
}

/* World -> body-fixed (rotation is body -> world). */
static dvec3 toBody(dmat4 rot, dvec3 w)
{
	dvec3 r;
	r.x = rot.m[0][0] * w.x + rot.m[1][0] * w.y + rot.m[2][0] * w.z;
	r.y = rot.m[0][1] * w.x + rot.m[1][1] * w.y + rot.m[2][1] * w.z;
	r.z = rot.m[0][2] * w.x + rot.m[1][2] * w.y + rot.m[2][2] * w.z;
	return r;
}

/* The dome's uniforms on the volume's programs (cloud_volume.h). */
static void volumeAir(GLuint program)
{
	Atmosphere_SetExtinction(program, (float)volScene.radius, Atmosphere_Params());
}

/* The clouds' sky and ground light by Sun height (cloudsky.frag), for the
   scene sc at layer altitude alt. Made every frame: 2 x 64 texels of 48
   short rays each, so the atmosphere's settings apply as they are edited. */
static GLuint skyTex, skyFBO;

static void updateSkyLight(CloudVolumeScene *sc, float alt)
{
	const int n = CLOUD_SKY_LUT_SIZE;
	if (!skyTex) {
		glGenTextures(1, &skyTex);
		glBindTexture(GL_TEXTURE_2D, skyTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, n, 2, 0, GL_RGBA, GL_FLOAT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glGenFramebuffers(1, &skyFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, skyFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, skyTex, 0);
	}

	GLint fbo, vp[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, skyFBO);
	glViewport(0, 0, n, 2);

	GLuint p = skyProgram;
	glUseProgram(p);
	volumeAir(p);
	glUniform1f(glGetUniformLocation(p, "lutSize"), (float)n);
	glUniform1f(glGetUniformLocation(p, "layerAlt"), alt);
	glUniform3f(glGetUniformLocation(p, "sunColor"), sc->sunColor[0], sc->sunColor[1], sc->sunColor[2]);
	glUniform1f(glGetUniformLocation(p, "sunAngle"), sc->sunAngle);
	glUniform3f(glGetUniformLocation(p, "groundAlbedo"), (float)CLOUD_GROUND_ALBEDO_R,
	            (float)CLOUD_GROUND_ALBEDO_G, (float)CLOUD_GROUND_ALBEDO_B);
	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	sc->skyLight = skyTex;
}

void Clouds_PrepareVolume(const FrameContext *f, int body)
{
	CloudVolumeScene *sc = &volScene;
	memset(sc, 0, sizeof *sc);
	sc->body = -1;
	if (body < 0 || !maps[body].ready)
		return;
	const cloudParams *c = paramsFor(body);
	updateCube(body);
	if (!CloudVolume_Params()->enabled)
		return;

	const Body *b = Bodies_Get(body);
	double R = b->radius;
	sc->fade = 1.0f;
	sc->body = body;
	sc->center = b->position;
	sc->radius = R;
	sc->rotation = b->rotation;
	/* The frame under the camera (it only places the shadow map). */
	dvec3 camB = toBody(b->rotation, dsubvec3(f->cam, b->position));
	dvec3 center = dnormal(camB);
	dvec3 pole = { 0.0, 0.0, 1.0 };
	dvec3 east = dcross(pole, center);
	east = dlengthvec3(east) > 1e-6 ? dnormal(east) : (dvec3){ 1.0, 0.0, 0.0 };
	sc->mapCenter = center;
	sc->mapEast = east;
	sc->mapNorth = dcross(center, east);

	sc->domeAir = 1;
	sc->weatherCube = cubeTex;
	sc->weatherCubeSize = CLOUD_CUBE_SIZE;
	sc->weatherTexelM = cubeTexelM(R);
	memcpy(sc->albedo, c->color, sizeof sc->albedo);
	sc->layerCount = 1;
	CloudVolumeLayer *L = &sc->layers[0];
	L->base = c->altitude - 0.5f * c->thickness;
	L->top = c->altitude + 0.5f * c->thickness;
	L->density = (float)(CLOUD_VOL_DENSITY * c->opacity / c->thickness);
	L->tint[0] = L->tint[1] = L->tint[2] = 1.0f;
	L->coverage = 1.0f;
	L->scale = (float)(CLOUD_VOL_SCALE * c->thickness);
	L->sweep = 1.0f;
	L->stretch = 1.0f;
	L->erosion = 0.35f + 0.5f * c->detail;

	/* The light: the Sun through the dome's air (quad.frag's sunlight on a
	   Lambert surface), its sky's light and the light bounced off the
	   ground, both from the dome's scattering (updateSkyLight). */
	sc->airUniforms = volumeAir;
	dvec3 toSun = dsubvec3(Bodies_Get(BODY_SUN)->position, f->cam);
	sc->sunDir = dnormal(toSun);
	sc->sunAngle = (float)SUN_ANGULAR_RADIUS_RAD;
	sc->sunColor[0] = (float)(1.0 * 1.15 / M_PI);
	sc->sunColor[1] = (float)(0.97 * 1.15 / M_PI);
	sc->sunColor[2] = (float)(0.90 * 1.15 / M_PI);
	sc->airScaleHeight = 1.0;
	sc->skyGain = (float)CLOUD_SKY_GAIN;
	sc->groundGain = (float)CLOUD_SKY_GAIN;
	sc->exposure = 1.0f;
	updateSkyLight(sc, c->altitude);

	CloudVolume_UpdateShadow(f, sc);
}

void Clouds_DrawVolume(const FrameContext *f)
{
	CloudVolume_Draw(f, &volScene);
}

float Clouds_ShellRadius(void)
{
	return volScene.body >= 0 ? (float)(volScene.radius + volScene.layers[0].top) : 0.0f;
}

void Clouds_BindStandIn(GLuint shader, int body)
{
	const CloudMaps *m = &maps[body];
	glUniform1f(glGetUniformLocation(shader, "cloudsOn"), m->ready ? 1.0f : 0.0f);
	if (!m->ready)
		return;
	const cloudParams *c = paramsFor(body);
	bindMaps(shader, m, 0, 1);
	glUniform1f(glGetUniformLocation(shader, "cloudOpacity"), c->opacity);
	glUniform1f(glGetUniformLocation(shader, "cloudDetail"), c->detail);
	glUniform1f(glGetUniformLocation(shader, "cloudEdge"), cloudEdge(c));
	glUniform3f(glGetUniformLocation(shader, "cloudColor"), c->color[0], c->color[1], c->color[2]);
}

/* ---- Debug panel (a tab of the engine/debug/debug_gui.c window) ---- */

/* The active planet's cloud layer (clouds.c). Every change rebuilds its
   weather maps and cube, so dragging a slider costs a few builds a frame. */
void Clouds_DebugPanel(struct nk_context *ctx)
{
	cloudParams *c = &g_clouds;

	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Clouds on (all planets; work in progress)", &g_cloudsEnabled);

	DebugGUI_RowLayout(ctx);
	nk_label(ctx, "Style", NK_TEXT_LEFT);
	nk_slider_int(ctx, 0, &c->style, CLOUD_STYLE_COUNT - 1, 1);
	nk_label(ctx, Clouds_StyleName(c->style), NK_TEXT_RIGHT);
	DebugGUI_Slider(ctx, "Coverage", &c->coverage, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Climate weight", &c->climateWeight, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Altitude (m)", &c->altitude, 500.0f, 1.5e5f, 100.0f, "%.0f");
	DebugGUI_Slider(ctx, "Thickness (m)", &c->thickness, 200.0f, 1.0e5f, 100.0f, "%.0f");
	DebugGUI_Slider(ctx, "Opacity (tau)", &c->opacity, 0.5f, 80.0f, 0.5f, "%.1f");
	DebugGUI_Slider(ctx, "Weather scale", &c->weatherScale, 0.1f, 1.5f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Sharpness", &c->sharpness, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Detail", &c->detail, 0.0f, 0.9f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Bands", &c->bands, 1.0f, 20.0f, 0.5f, "%.1f");
	/* Written back only when moved: the round trip through days can change
	   the last bit, and any change rebuilds the maps. */
	float days = c->flowPeriod / 86400.0f, shown = days;
	DebugGUI_Slider(ctx, "Flow period (d)", &days, 0.0f, 10.0f, 0.05f, "%.2f");
	if (days != shown)
		c->flowPeriod = days * 86400.0f;
	DebugGUI_Slider(ctx, "Colour red", &c->color[0], 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Colour green", &c->color[1], 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Colour blue", &c->color[2], 0.0f, 1.0f, 0.01f, "%.2f");

	DebugGUI_Group(ctx, "Quality (all planets)");
	DebugGUI_SliderInt(ctx, "Detail octaves", &g_cloudDetailOctaves, 0, MAX_DETAIL_OCTAVES);

	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	if (nk_button_label(ctx, "Reset clouds (Earth)"))
		*c = Clouds_Defaults();

	CloudVolume_DebugPanel(ctx);
}
