#include "world/gasgiant/gas_interior.h"
#include "world/gasgiant/gas_giant.h"
#include "world/bodies/bodies.h"
#include "world/bodies/solar_system.h"
#include "world/clouds/cloud_volume.h"
#include "engine/gfx/shader.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <string.h>

/* Texture unit for the scene depth (the stand-in program's units are not
   in use here). */
#define UNIT_DEPTH 4
/* March samples per ray. */
#define HAZE_STEPS 32

static GLuint program, emptyVAO;
/* The weather bake (gasweather.frag), and the style and body it was last
   baked for. */
static GLuint bakeProgram;
static unsigned int bakedHash;
static int bakedBody = -1;
static dvec3 bakedCenter, bakedEast, bakedNorth;   /* the map's frame, body-fixed */
static GLuint sceneDepthTex;
static int sceneDepthSamples;
static float sceneNear;
static unsigned int frameCount;

static GasInteriorParams g_params = {
	1,        /* enabled */
	0.45f,    /* ambient */
	0.5f,     /* rays */
	1.0f,     /* glare */
	1.0f,     /* murk */
};

GasInteriorParams *GasInterior_Params(void) { return &g_params; }

void GasInterior_Init(void)
{
	Shader_Create(&program, "world/gasgiant/gasinterior.vert", "world/gasgiant/gasinterior.frag");
	Shader_Create(&bakeProgram, "world/gasgiant/gasinterior.vert", "world/gasgiant/gasweather.frag");
	GasGiant_AttachStandIn(bakeProgram);
	glGenVertexArrays(1, &emptyVAO);
}

void GasInterior_SetSceneDepth(GLuint depthTex, int samples, float nearPlane)
{
	sceneDepthTex = depthTex;
	sceneDepthSamples = samples;
	sceneNear = nearPlane;
}

static double smoothstepd(double e0, double e1, double x)
{
	double t = (x - e0) / (e1 - e0);
	t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
	return t * t * (3.0 - 2.0 * t);
}

int GasInterior_Body(dvec3 cam, double *altitude)
{
	int best = -1;
	double bestAlt = GAS_FLIGHT_FULL_SPEED_ALT_M;
	for (int i = 0; i < BODY_COUNT; i++) {
		if (SolarSystem_Info(i)->type != PLANET_GAS)
			continue;
		const Body *b = Bodies_Get(i);
		double alt = dlengthvec3(dsubvec3(cam, b->position)) - b->radius;
		if (alt < bestAlt) {
			bestAlt = alt;
			best = i;
		}
	}
	if (altitude)
		*altitude = bestAlt;
	return best;
}

double GasInterior_SpeedScale(double altitude)
{
	double s = altitude / GAS_FLIGHT_FULL_SPEED_ALT_M;
	return s < GAS_FLIGHT_MIN_SPEED_SCALE ? GAS_FLIGHT_MIN_SPEED_SCALE : s > 1.0 ? 1.0 : s;
}

double GasInterior_FloorRadius(int id)
{
	return Bodies_Get(id)->radius - GAS_INTERIOR_FLOOR_M;
}

double GasInterior_ScaleHeight(const GasGiantStyle *s, double radius)
{
	return fmax(s->shellFraction * radius / GAS_HAZE_TOP_H, GAS_HAZE_SCALE_HEIGHT_MIN_M);
}

float GasInterior_StandInHaze(double H, double altitude)
{
	if (!g_params.enabled)
		return 1.0f;
	return (float)smoothstepd(GAS_INTERIOR_FADE_LO_H * H, GAS_INTERIOR_FADE_HI_H * H, altitude);
}

static void uniform3(const char *name, double x, double y, double z)
{
	glUniform3f(glGetUniformLocation(program, name), (float)x, (float)y, (float)z);
}

/* World direction -> view axes (the rows of the rotation-only view). */
static dvec3 toView(mat4 v, dvec3 w)
{
	dvec3 r;
	r.x = v.m[0][0] * w.x + v.m[0][1] * w.y + v.m[0][2] * w.z;
	r.y = v.m[1][0] * w.x + v.m[1][1] * w.y + v.m[1][2] * w.z;
	r.z = v.m[2][0] * w.x + v.m[2][1] * w.y + v.m[2][2] * w.z;
	return r;
}

void GasInterior_Draw(const FrameContext *f)
{
	frameCount++;
	double alt;
	int id = GasInterior_Body(f->cam, &alt);
	if (id < 0)
		return;
	const Body *b = Bodies_Get(id);
	const GasGiantStyle *s = &SolarSystem_Info(id)->gas;
	double H = GasInterior_ScaleHeight(s, b->radius);
	float share = 1.0f - GasInterior_StandInHaze(H, alt);
	if (share <= 0.0f)
		return;

	const GasGiantParams *g = GasGiant_Params();
	dvec3 rel = dsubvec3(f->cam, b->position);
	double r = dlengthvec3(rel);
	dvec3 up = dscalarvec3(rel, 1.0 / r);
	dvec3 toSun = dsubvec3(Bodies_Get(BODY_SUN)->position, f->cam);
	toSun = dscalarvec3(toSun, 1.0 / dlengthvec3(toSun));
	dvec3 upV = toView(f->view, up), sunV = toView(f->view, toSun);

	glUseProgram(program);
	glUniformMatrix4fv(glGetUniformLocation(program, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glUniform2f(glGetUniformLocation(program, "viewportSize"), (float)viewport[2], (float)viewport[3]);
	uniform3("camUp", upV.x, upV.y, upV.z);
	glUniform1f(glGetUniformLocation(program, "camSun"), (float)(up.x * toSun.x + up.y * toSun.y + up.z * toSun.z));
	uniform3("sunDir", sunV.x, sunV.y, sunV.z);
	glUniform1f(glGetUniformLocation(program, "radius"), (float)b->radius);
	glUniform1f(glGetUniformLocation(program, "camAlt"), (float)alt);
	glUniform1f(glGetUniformLocation(program, "scaleHeight"), (float)H);
	/* The stand-in's veil is the whole column (hazeTau) straight down:
	   the same column, spread exponentially over the scale height. */
	glUniform1f(glGetUniformLocation(program, "sigma0"), (float)(s->hazeTau / H));
	glUniform1f(glGetUniformLocation(program, "hazeTop"), (float)(GAS_HAZE_TOP_H * H));
	glUniform1f(glGetUniformLocation(program, "murkSigma"), (float)(GAS_DECK_MURK_PER_M * g_params.murk));
	uniform3("hazeColor", s->hazeColor[0], s->hazeColor[1], s->hazeColor[2]);
	uniform3("murkColor", s->belt[0], s->belt[1], s->belt[2]);
	uniform3("terminator", g->terminator[0], g->terminator[1], g->terminator[2]);
	glUniform1f(glGetUniformLocation(program, "wrap"), g->wrap);
	glUniform1f(glGetUniformLocation(program, "rim"), g->rim);
	glUniform1f(glGetUniformLocation(program, "nightAmbient"), g->ambient);
	glUniform1f(glGetUniformLocation(program, "exposure"), g->exposure);
	glUniform1f(glGetUniformLocation(program, "ambient"), g_params.ambient);
	glUniform1f(glGetUniformLocation(program, "rays"), g_params.rays);
	glUniform1f(glGetUniformLocation(program, "glare"), g_params.glare);
	glUniform1f(glGetUniformLocation(program, "share"), share);
	glUniform1i(glGetUniformLocation(program, "steps"), HAZE_STEPS);
	glUniform1f(glGetUniformLocation(program, "frameJitter"), (float)fmod(frameCount * 0.61803398875, 1.0));

	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneDepthTex);
	glUniform1i(glGetUniformLocation(program, "sceneDepth"), UNIT_DEPTH);
	glUniform1i(glGetUniformLocation(program, "sceneSamples"), sceneDepthTex ? sceneDepthSamples : 0);
	glUniform1f(glGetUniformLocation(program, "depthNear"), sceneNear);
	glActiveTexture(GL_TEXTURE0);

	/* Premultiplied over the scene (in-scatter, 1 - transmittance); the
	   march ends at the depth it reads, so no depth test or writes. The
	   alpha channel keeps the cloud cover the Sun's glow read. */
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	if (cull)
		glEnable(GL_CULL_FACE);

	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glActiveTexture(GL_TEXTURE0);
}

double GasInterior_CloudReach(double H, double altitude)
{
	return CloudVolume_Reach(altitude - GAS_CLOUD_MID_BASE_H * H, GAS_CLOUD_MAP_EXTENT_M);
}

float GasInterior_VolumeFade(double H, double altitude)
{
	if (!g_params.enabled || !CloudVolume_Params()->enabled)
		return 0.0f;
	return 1.0f - (float)smoothstepd(GAS_CLOUD_FADE_LO_H * H, GAS_CLOUD_FADE_HI_H * H, altitude);
}

static double luminance(const float c[3])
{
	return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}

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

/* The weather map under the camera, from the exterior's cloud field. */
static void bakeWeather(int id, GasGiantStyle *s, const Body *b, double alt, dvec3 center, dvec3 east, dvec3 north)
{
	CloudVolume_BeginWeatherBake();
	glUseProgram(bakeProgram);
	GasGiant_BindStandIn(bakeProgram, s, b->radius, alt);
	glUniform3f(glGetUniformLocation(bakeProgram, "mapCenter"), (float)center.x, (float)center.y, (float)center.z);
	glUniform3f(glGetUniformLocation(bakeProgram, "mapEast"), (float)east.x, (float)east.y, (float)east.z);
	glUniform3f(glGetUniformLocation(bakeProgram, "mapNorth"), (float)north.x, (float)north.y, (float)north.z);
	glUniform1f(glGetUniformLocation(bakeProgram, "mapExtent"), (float)GAS_CLOUD_MAP_EXTENT_M);
	glUniform1f(glGetUniformLocation(bakeProgram, "mapSize"), (float)CLOUDVOL_MAP_SIZE);
	glUniform1f(glGetUniformLocation(bakeProgram, "radius"), (float)b->radius);
	glUniform1f(glGetUniformLocation(bakeProgram, "lumZone"), (float)luminance(s->zone));
	glUniform1f(glGetUniformLocation(bakeProgram, "lumBelt"), (float)luminance(s->belt));
	glDrawArrays(GL_TRIANGLES, 0, 3);
	CloudVolume_EndWeatherBake(id, center);
	bakedHash = s->profileHash;
	bakedBody = id;
	bakedCenter = center;
	bakedEast = east;
	bakedNorth = north;
}

void GasInterior_DrawClouds(const FrameContext *f)
{
	double alt;
	int id = GasInterior_Body(f->cam, &alt);
	if (id < 0)
		return;
	const Body *b = Bodies_Get(id);
	GasGiantStyle *s = &SolarSystem_Info(id)->gas;
	double H = GasInterior_ScaleHeight(s, b->radius);
	CloudVolumeScene sc;
	memset(&sc, 0, sizeof sc);
	sc.fade = GasInterior_VolumeFade(H, alt);
	if (sc.fade <= 0.0f) {
		CloudVolume_Draw(f, &sc);   /* drops the history */
		return;
	}

	/* The map's frame: under the camera, east and north there (pole +Z). */
	dvec3 camB = toBody(b->rotation, dsubvec3(f->cam, b->position));
	dvec3 center = dnormal(camB);
	dvec3 pole = { 0.0, 0.0, 1.0 };
	dvec3 east = dcross(pole, center);
	east = dlengthvec3(east) > 1e-6 ? dnormal(east) : (dvec3){ 1.0, 0.0, 0.0 };
	dvec3 north = dcross(center, east);
	/* Re-bake when the camera has moved on, or the style changed (the
	   stand-in pass, just before, rebuilt its profile). */
	if (CloudVolume_NeedsBake(id, camB, b->radius, GAS_CLOUD_MAP_EXTENT_M) ||
	    bakedBody != id || bakedHash != s->profileHash)
		bakeWeather(id, s, b, alt, center, east, north);

	const GasGiantParams *g = GasGiant_Params();
	sc.body = id;
	sc.center = b->position;
	sc.radius = b->radius;
	sc.rotation = b->rotation;
	/* The frame the map was baked in (up to an eighth of it away). */
	sc.mapCenter = bakedCenter;
	sc.mapEast = bakedEast;
	sc.mapNorth = bakedNorth;
	sc.mapExtent = GAS_CLOUD_MAP_EXTENT_M;
	sc.layerCount = 2;

	/* Layer 0, top: bright ammonia, thin, swept. Layer 1, middle: rust,
	   tall, towering where the map says. Altitudes in scale heights. */
	CloudVolumeLayer *top = &sc.layers[0], *mid = &sc.layers[1];
	top->base = (float)(GAS_CLOUD_TOP_BASE_H * H);
	top->top = (float)(GAS_CLOUD_TOP_TOP_H * H);
	top->density = 1.0f / 4000.0f;
	top->tint[0] = 1.2f; top->tint[1] = 1.18f; top->tint[2] = 1.12f;
	top->coverage = 1.0f;
	top->scale = 90.0e3f;
	top->sweep = 3.0f;
	top->shear = 0.6f;
	top->erosion = 1.0f;
	mid->base = (float)(GAS_CLOUD_MID_BASE_H * H);
	mid->top = (float)(GAS_CLOUD_MID_TOP_H * H);
	mid->density = 1.0f / 500.0f;
	mid->tint[0] = 1.05f; mid->tint[1] = 0.98f; mid->tint[2] = 0.92f;
	mid->coverage = 1.0f;
	mid->scale = 30.0e3f;
	mid->sweep = 1.5f;
	mid->shear = 0.3f;
	mid->erosion = 1.0f;

	dvec3 toSun = dsubvec3(Bodies_Get(BODY_SUN)->position, f->cam);
	sc.sunDir = dnormal(toSun);
	sc.airColumn = s->hazeTau;
	sc.airScaleHeight = H;
	for (int c = 0; c < 3; c++)
		sc.terminator[c] = g->terminator[c];
	sc.wrap = g->wrap;
	memcpy(sc.airAlbedo, s->hazeColor, sizeof sc.airAlbedo);
	sc.airAmbient = g_params.ambient;
	sc.airNight = g->ambient;
	sc.airForward = fminf(fmaxf(0.25f * g->rim, 0.0f), 1.0f);
	memcpy(sc.skyColor, s->hazeColor, sizeof sc.skyColor);
	sc.skyGain = g_params.ambient;
	memcpy(sc.groundColor, s->belt, sizeof sc.groundColor);
	sc.groundGain = 0.3f;
	sc.exposure = g->exposure;
	CloudVolume_Draw(f, &sc);
}

void GasInterior_DebugPanel(struct nk_context *ctx)
{
	DebugGUI_Group(ctx, "Interior (all giants)");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Interior haze pass", &g_params.enabled);
	DebugGUI_Slider(ctx, "Skylight", &g_params.ambient, 0.0f, 2.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Sun rays", &g_params.rays, 0.0f, 2.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Sun glare", &g_params.glare, 0.0f, 5.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Murk density", &g_params.murk, 0.0f, 5.0f, 0.01f, "%.2f");
}
