#include "Common.h"
#include "Clouds.h"
#include "SolarSystem.h"
#include "Atmosphere.h"
#include "Rings.h"
#include "Window.h"
#include "WorldConfig.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Earth's layer (WorldConfig.h). A macro so the static initializer and
   cloudDefaults() share one copy. Colour: a slightly warm white. */
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

cloudParams g_clouds = CLOUD_DEFAULTS;
int g_cloudSlabOctaves = CLOUD_SLAB_OCTAVES;
int g_cloudsEnabled = CLOUDS_ENABLED_DEFAULT;

cloudParams cloudDefaults(void)
{
	cloudParams c = CLOUD_DEFAULTS;
	return c;
}

const char *Clouds_StyleName(int style)
{
	static const char *names[CLOUD_STYLE_COUNT] = { "None", "Climate", "Patchy", "Banded", "Overcast" };
	return style >= 0 && style < CLOUD_STYLE_COUNT ? names[style] : "?";
}

/* Texture units. Receivers (quad / water): 0-13 are taken. The slab and
   the stand-ins have their own programs: 0-2, and the ring profile on 9
   (Rings_BindReceiver). */
#define RECEIVER_UNIT_A 14
#define RECEIVER_UNIT_B 15
#define SLAB_UNIT_A     0
#define SLAB_UNIT_B     1
#define SLAB_UNIT_WIND  2
#define SLAB_UNIT_NOISE 3
#define MAP_UNIT_NOISE  2
/* Value-noise lattice (cloudnoise.glsl): texels per side, repeating. */
#define CLOUD_NOISE_SIZE 256
/* TerrainGen.c's ocean map stores wind speed / WIND_MAX_MS. */
#define WIND_MAP_MAX_MS 20.0f

static GLuint mapProgram;
/* The slab, one program per detail octave count (SLAB_OCTAVES), built when
   first needed: the count is a compile-time loop bound (clouds.frag). */
#define MAX_SLAB_OCTAVES 12
static GLuint slabPrograms[MAX_SLAB_OCTAVES + 1];
static GLuint mapFBO, emptyVAO, noiseTex;
static int mapW, mapH;
static float mapMaxLod;

/* A planet's two newest weather maps: tex[0] at step * CLOUD_MAP_STEP_S,
   tex[1] one step later, and the settings they were built with. */
typedef struct CloudMaps {
	GLuint tex[2];
	long long step;
	int ready;
	cloudParams built;
} CloudMaps;
static CloudMaps maps[BODY_COUNT];
static double frameTime;

/* The active planet's state is in the globals; the rest in PlanetInfo. */
static const cloudParams *paramsFor(int body)
{
	return body == SolarSystem_ActiveBody() ? &g_clouds : &g_planetInfo[body].clouds;
}

static const PlanetGenParams *genFor(int body)
{
	return body == SolarSystem_ActiveBody() ? &g_planetGen : &g_planetInfo[body].gen;
}

static int wantsClouds(int body)
{
	const PlanetGenParams *g = genFor(body);
	return g_cloudsEnabled && g_planetInfo[body].baked && paramsFor(body)->style != CLOUD_STYLE_NONE
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
	createShader(&mapProgram, "cloudmap.vert", "cloudmap.frag");

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
	double R = g_bodies[body].radius;

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

/* ---- Uniforms shared by the slab, receivers and stand-ins -------------- */

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

/* Half-width of a cloud edge in coverage-field units: soft to hard. */
static float cloudEdge(const cloudParams *c)
{
	return 0.06f + (0.004f - 0.06f) * c->sharpness;
}

/* Radians per screen pixel at the view centre. */
static float pixelAngle(void)
{
	return (float)(2.0 * tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0) / getWindowHeight());
}

/* cloudlayer.glsl's uniforms. */
static void setLayerUniforms(GLuint s, int body, int unitA, int unitB)
{
	const CloudMaps *m = &maps[body];
	glUniform1f(glGetUniformLocation(s, "cloudsOn"), m->ready ? 1.0f : 0.0f);
	if (!m->ready)
		return;
	const cloudParams *c = paramsFor(body);
	const Body *b = &g_bodies[body];
	bindMaps(s, m, unitA, unitB);
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
	glUniform1f(glGetUniformLocation(s, "cloudDetail"), c->detail);
	glUniform1f(glGetUniformLocation(s, "cloudEdge"), cloudEdge(c));
	glUniform1f(glGetUniformLocation(s, "cloudPixAngle"), pixelAngle());
}

void Clouds_BindReceiver(GLuint shader, int body)
{
	setLayerUniforms(shader, body, RECEIVER_UNIT_A, RECEIVER_UNIT_B);
}

/* World vector -> view space (the view matrix's rows are the camera axes). */
static dvec3 toView(mat4 view, dvec3 w)
{
	dvec3 v;
	v.x = view.m[0][0] * w.x + view.m[0][1] * w.y + view.m[0][2] * w.z;
	v.y = view.m[1][0] * w.x + view.m[1][1] * w.y + view.m[1][2] * w.z;
	v.z = view.m[2][0] * w.x + view.m[2][1] * w.y + view.m[2][2] * w.z;
	return v;
}

/* The first detail octave's wavelength: ~4 map texels at the layer. */
static double detailLambda0(int body)
{
	return 4.0 * 2.0 * M_PI * (g_bodies[body].radius + paramsFor(body)->altitude) / mapW;
}

/* Detail octaves for body: g_cloudSlabOctaves, fewer where they would go
   below CLOUD_DETAIL_MIN_WAVELENGTH_M. */
static int slabOctaves(int body)
{
	int want = g_cloudSlabOctaves < 0 ? 0 : (g_cloudSlabOctaves > MAX_SLAB_OCTAVES ? MAX_SLAB_OCTAVES : g_cloudSlabOctaves);
	double lambda0 = detailLambda0(body);
	int octaves = want > 0 ? 1 : 0;
	while (octaves < want && lambda0 / pow(2.0, octaves) >= CLOUD_DETAIL_MIN_WAVELENGTH_M)
		octaves++;
	return octaves;
}

static GLuint slabProgram(int octaves)
{
	if (!slabPrograms[octaves]) {
		char defines[64];
		snprintf(defines, sizeof defines, "#define SLAB_OCTAVES %d\n", octaves);
		createShaderDefines(&slabPrograms[octaves], "clouds.vert", "clouds.frag", defines);
		Rings_AttachReceiver(slabPrograms[octaves]);
	}
	return slabPrograms[octaves];
}

/* Everything the slab program needs; it is bound by the caller. */
static void setSlabUniforms(GLuint s, mat4 proj, mat4 view, dvec3 cam, int body)
{
	const cloudParams *c = paramsFor(body);
	const Body *b = &g_bodies[body];
	double R = b->radius, Rc = R + c->altitude;
	dvec3 rel = dsubvec3(b->position, cam);
	double d = dlengthvec3(rel);
	dvec3 cv = toView(view, rel);

	/* A quad around the layer's silhouette while the planet sits well in
	   front of the camera; otherwise the plane through its centre would
	   clip, so the whole screen. */
	int fullScreen = d < 1.5 * Rc || cv.z > -Rc;
	glUniform1i(glGetUniformLocation(s, "fullScreen"), fullScreen);
	glUniform3f(glGetUniformLocation(s, "centerView"), (float)cv.x, (float)cv.y, (float)cv.z);
	glUniform1f(glGetUniformLocation(s, "halfSize"), fullScreen ? 0.0f : (float)(Rc * d / sqrt(d * d - Rc * Rc)));

	glUniformMatrix4fv(glGetUniformLocation(s, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glUniform2f(glGetUniformLocation(s, "viewportSize"), (float)viewport[2], (float)viewport[3]);
	/* View -> world: column j is camera axis j (row j of the view matrix). */
	float v2w[9];
	for (int j = 0; j < 3; j++)
		for (int k = 0; k < 3; k++)
			v2w[j * 3 + k] = view.m[j][k];
	glUniformMatrix3fv(glGetUniformLocation(s, "viewToWorld"), 1, GL_FALSE, v2w);
	glUniform3f(glGetUniformLocation(s, "camPlanet"), (float)(-rel.x / R), (float)(-rel.y / R), (float)(-rel.z / R));
	glUniform1f(glGetUniformLocation(s, "planetRadius"), (float)R);
	dvec3 toSun = dsubvec3(g_bodies[BODY_SUN].position, b->position);
	toSun = dscalarvec3(toSun, 1.0 / dlengthvec3(toSun));
	glUniform3f(glGetUniformLocation(s, "sunDir"), (float)toSun.x, (float)toSun.y, (float)toSun.z);
	glUniform1f(glGetUniformLocation(s, "sunAngle"), (float)SUN_ANGULAR_RADIUS_RAD);

	setLayerUniforms(s, body, SLAB_UNIT_A, SLAB_UNIT_B);
	FlowPhases f = flowAt(frameTime, c);
	setFlowUniforms(s, &f);

	glUniform1f(glGetUniformLocation(s, "detailLambda0"), (float)detailLambda0(body));
	glUniform3f(glGetUniformLocation(s, "cloudColor"), c->color[0], c->color[1], c->color[2]);
	glActiveTexture(GL_TEXTURE0 + SLAB_UNIT_WIND);
	glBindTexture(GL_TEXTURE_CUBE_MAP, genFor(body)->oceanTex);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(s, "cloudWind"), SLAB_UNIT_WIND);
	bindNoise(s, SLAB_UNIT_NOISE);
	glUniform1f(glGetUniformLocation(s, "windMax"), WIND_MAP_MAX_MS);

	/* The air (extinction toward the camera, sunlight) and ring shadows. */
	setAtmosphereExtinction(s, (float)R, &g_atmosphere);
	Rings_BindReceiver(s);
}

void Clouds_Draw(mat4 proj, mat4 view, dvec3 cam, int body)
{
	if (!maps[body].ready)
		return;

	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GREATER);   /* reversed-Z */
	glBindVertexArray(emptyVAO);

	/* Blended over the scene in display space, depth-tested against it
	   (terrain above the layer, the planet in front of the far side), no
	   depth writes. The cloud's alpha also lands in the target's alpha
	   (cleared to 0 before, Main.c): the dome adds its in-scatter times
	   (1 - alpha), the cloud having added the air in front of itself. */
	GLuint prog = slabProgram(slabOctaves(body));
	glUseProgram(prog);
	setSlabUniforms(prog, proj, view, cam, body);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);

	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
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
