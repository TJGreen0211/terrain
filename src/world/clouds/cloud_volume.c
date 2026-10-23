#include "world/clouds/cloud_volume.h"
#include "engine/gfx/shader.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Shape noise: NOISE_SHAPE_N^3 texels, Perlin-Worley in r and Worley FBM
   at rising frequencies in g, b, a. Detail noise: NOISE_DETAIL_N^3 texels,
   Worley FBM in r, g, b. Both tile. */
#define NOISE_SHAPE_N  128
#define NOISE_DETAIL_N 32

/* Texture units. */
#define UNIT_WEATHER 0
#define UNIT_COLOR   1
#define UNIT_SHAPE   2
#define UNIT_DETAIL  3
#define UNIT_DEPTH   4
#define UNIT_SHADOW  5

/* Shadow map: samples along each texel's ray through the shell. */
#define SHADOW_STEPS 24

static GLuint marchProgram, resolveProgram, compositeProgram, shadowProgram, emptyVAO;
static GLuint shapeTex, detailTex;
static GLuint weatherTex, colorTex, mapFBO;
/* Low-resolution targets: this frame's march, and two accumulated
   histories (ping-pong), each colour (premultiplied radiance, alpha) and
   depth (distance along the ray, m). */
static GLuint marchColor, marchDepth, marchFBO;
static GLuint histColor[2], histDepth[2], histFBO[2];
static int lowW, lowH, histIndex;
static GLuint sceneDepthTex;
static int sceneDepthSamples;
static float sceneNear;

/* The weather map: valid for which body, centred where (body-fixed). */
static int mapValid, mapBody = -1;
static dvec3 mapCenterB;
static GLint savedFBO, savedViewport[4];

/* Last frame, for the reprojection. */
static int prevValid, prevBody = -1;
static dvec3 prevCamB;
static float prevViewToBody[9];
static unsigned int frameCount;

/* The shadow map and the frame it was made in (body-fixed, m). */
static GLuint shadowTex, shadowFBO;
static int shadowValid;
static dmat4 shadowRotation;
static dvec3 shadowBodyCenter, shadowMapCenter, shadowEast, shadowNorth, shadowSunB;
static double shadowCX, shadowCY, shadowExtentM;
static float shadowLo, shadowHi;

static CloudVolumeParams g_params = {
	1,          /* enabled */
	2,          /* resDivisor */
	96,         /* viewSteps */
	5,          /* lightSteps */
	400.0e3f,   /* maxDistance */
	0.9f,       /* history */
	0.18f,      /* detailScale */
	0.5f,       /* multiScatter */
	0.5f,       /* powder */
	0.6f,       /* ambient */
	300.0e3f,   /* shadowExtent */
};

CloudVolumeParams *CloudVolume_Params(void) { return &g_params; }

/* ---- Noise volumes (CPU, once) ------------------------------------------- */

static unsigned int hash3(int x, int y, int z, unsigned int seed)
{
	unsigned int h = seed * 0x9e3779b9u;
	h ^= (unsigned int)x * 0x85ebca6bu;
	h = (h << 13) | (h >> 19);
	h ^= (unsigned int)y * 0xc2b2ae35u;
	h = (h << 13) | (h >> 19);
	h ^= (unsigned int)z * 0x27d4eb2fu;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	h *= 0x297a2d39u;
	h ^= h >> 15;
	return h;
}

static float u01(unsigned int h) { return (float)(h & 0xffffff) / 16777216.0f; }

static int wrapi(int i, int n) { i %= n; return i < 0 ? i + n : i; }

static float fadef(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/* Tiling gradient noise, period n cells, p in cells; about [-1, 1]. */
static float perlin(float px, float py, float pz, int n, unsigned int seed)
{
	int ix = (int)floorf(px), iy = (int)floorf(py), iz = (int)floorf(pz);
	float fx = px - ix, fy = py - iy, fz = pz - iz;
	float v[8];
	for (int c = 0; c < 8; c++) {
		int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
		unsigned int h = hash3(wrapi(ix + dx, n), wrapi(iy + dy, n), wrapi(iz + dz, n), seed);
		float gx = u01(h) * 2.0f - 1.0f;
		float gy = u01(h * 0x45d9f3bu + 1u) * 2.0f - 1.0f;
		float gz = u01(h * 0x119de1f3u + 7u) * 2.0f - 1.0f;
		float l = sqrtf(gx * gx + gy * gy + gz * gz) + 1e-6f;
		v[c] = (gx * (fx - dx) + gy * (fy - dy) + gz * (fz - dz)) / l;
	}
	float ux = fadef(fx), uy = fadef(fy), uz = fadef(fz);
	float a = v[0] + (v[1] - v[0]) * ux, b = v[2] + (v[3] - v[2]) * ux;
	float c2 = v[4] + (v[5] - v[4]) * ux, d = v[6] + (v[7] - v[6]) * ux;
	float e = a + (b - a) * uy, f = c2 + (d - c2) * uy;
	return (e + (f - e) * uz) * 1.5f;
}

/* Tiling Worley (distance to the nearest feature point, one per cell),
   period n cells, p in cells; inverted: 1 at a feature point. */
static float worley(float px, float py, float pz, int n, unsigned int seed)
{
	int ix = (int)floorf(px), iy = (int)floorf(py), iz = (int)floorf(pz);
	float best = 1e9f;
	for (int dz = -1; dz <= 1; dz++)
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++) {
				int cx = ix + dx, cy = iy + dy, cz = iz + dz;
				unsigned int h = hash3(wrapi(cx, n), wrapi(cy, n), wrapi(cz, n), seed);
				float fx = cx + u01(h) - px;
				float fy = cy + u01(h * 0x45d9f3bu + 3u) - py;
				float fz = cz + u01(h * 0x119de1f3u + 5u) - pz;
				float d2 = fx * fx + fy * fy + fz * fz;
				if (d2 < best)
					best = d2;
			}
	float d = sqrtf(best);
	return 1.0f - (d > 1.0f ? 1.0f : d);
}

/* Worley FBM: three octaves from n cells. u in [0, 1). */
static float worleyFbm(float ux, float uy, float uz, int n, unsigned int seed)
{
	return worley(ux * n, uy * n, uz * n, n, seed) * 0.625f
	     + worley(ux * 2 * n, uy * 2 * n, uz * 2 * n, 2 * n, seed + 1u) * 0.25f
	     + worley(ux * 4 * n, uy * 4 * n, uz * 4 * n, 4 * n, seed + 2u) * 0.125f;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static float remapf(float v, float lo, float hi, float nlo, float nhi)
{
	return nlo + (v - lo) / (hi - lo) * (nhi - nlo);
}

static unsigned char byte01(float v) { return (unsigned char)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

static GLuint newNoise3D(int n, const unsigned char *data)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_3D, tex);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glGenerateMipmap(GL_TEXTURE_3D);
	glBindTexture(GL_TEXTURE_3D, 0);
	return tex;
}

static void buildNoise(void)
{
	int n = NOISE_SHAPE_N;
	unsigned char *data = malloc((size_t)n * n * n * 4);
	int z;
#pragma omp parallel for schedule(dynamic)
	for (z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				float ux = (x + 0.5f) / n, uy = (y + 0.5f) / n, uz = (z + 0.5f) / n;
				/* Perlin FBM, 4 octaves from 4 cells, to [0, 1]. */
				float p = 0.0f, a = 0.5f, total = 0.0f;
				for (int o = 0; o < 4; o++) {
					int c = 4 << o;
					p += a * perlin(ux * c, uy * c, uz * c, c, 11u + o);
					total += a;
					a *= 0.5f;
				}
				p = clampf(p / total * 0.5f + 0.5f, 0.0f, 1.0f);
				float w4 = worleyFbm(ux, uy, uz, 4, 21u);
				/* Perlin-Worley: billowy Perlin, cut by the Worley cells. */
				float pw = clampf(remapf(p, w4 - 1.0f, 1.0f, 0.0f, 1.0f), 0.0f, 1.0f);
				unsigned char *t = data + (((size_t)z * n + y) * n + x) * 4;
				t[0] = byte01(pw);
				t[1] = byte01(worleyFbm(ux, uy, uz, 4, 31u));
				t[2] = byte01(worleyFbm(ux, uy, uz, 8, 41u));
				t[3] = byte01(worleyFbm(ux, uy, uz, 16, 51u));
			}
	shapeTex = newNoise3D(n, data);
	free(data);

	n = NOISE_DETAIL_N;
	data = malloc((size_t)n * n * n * 4);
#pragma omp parallel for schedule(dynamic)
	for (z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				float ux = (x + 0.5f) / n, uy = (y + 0.5f) / n, uz = (z + 0.5f) / n;
				unsigned char *t = data + (((size_t)z * n + y) * n + x) * 4;
				t[0] = byte01(worleyFbm(ux, uy, uz, 2, 61u));
				t[1] = byte01(worleyFbm(ux, uy, uz, 4, 71u));
				t[2] = byte01(worleyFbm(ux, uy, uz, 8, 81u));
				t[3] = 255;
			}
	detailTex = newNoise3D(n, data);
	free(data);
}

/* ---- Targets ------------------------------------------------------------ */

static GLuint newTex2D(int w, int h, GLint internal, GLenum format, GLenum type, GLint filter)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

static GLuint newFBO2(GLuint color, GLuint depth)
{
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depth, 0);
	GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
	return fbo;
}

static GLuint newFBO1(GLuint color)
{
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	GLenum buf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &buf);
	return fbo;
}

static void freeLowTargets(void)
{
	if (!marchFBO)
		return;
	glDeleteFramebuffers(1, &marchFBO);
	glDeleteFramebuffers(2, histFBO);
	glDeleteTextures(1, &marchColor);
	glDeleteTextures(1, &marchDepth);
	glDeleteTextures(2, histColor);
	glDeleteTextures(2, histDepth);
	marchFBO = 0;
}

/* (Re)makes the low-resolution targets for this size; 1 if they are new
   (the history is then empty). */
static int ensureLowTargets(int w, int h)
{
	if (marchFBO && w == lowW && h == lowH)
		return 0;
	freeLowTargets();
	lowW = w;
	lowH = h;
	marchColor = newTex2D(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
	marchDepth = newTex2D(w, h, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
	marchFBO = newFBO2(marchColor, marchDepth);
	for (int i = 0; i < 2; i++) {
		histColor[i] = newTex2D(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_LINEAR);
		histDepth[i] = newTex2D(w, h, GL_R32F, GL_RED, GL_FLOAT, GL_NEAREST);
		histFBO[i] = newFBO2(histColor[i], histDepth[i]);
	}
	return 1;
}

void CloudVolume_Init(void)
{
	Shader_Create(&marchProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudvolume.frag");
	Shader_Create(&resolveProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudresolve.frag");
	Shader_Create(&compositeProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudcomposite.frag");
	Shader_Create(&shadowProgram, "world/clouds/cloudvol.vert", "world/clouds/cloudshadow.frag");
	glGenVertexArrays(1, &emptyVAO);
	buildNoise();

	weatherTex = newTex2D(CLOUDVOL_MAP_SIZE, CLOUDVOL_MAP_SIZE, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	colorTex = newTex2D(CLOUDVOL_MAP_SIZE, CLOUDVOL_MAP_SIZE, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	mapFBO = newFBO2(weatherTex, colorTex);
	shadowTex = newTex2D(CLOUDVOL_SHADOW_SIZE, CLOUDVOL_SHADOW_SIZE, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_LINEAR);
	shadowFBO = newFBO1(shadowTex);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CloudVolume_SetSceneDepth(GLuint depthTex, int samples, float nearPlane)
{
	sceneDepthTex = depthTex;
	sceneDepthSamples = samples;
	sceneNear = nearPlane;
}

double CloudVolume_Reach(double aboveLayers, double mapExtent)
{
	return fmin(fmax(g_params.maxDistance, 2.5 * aboveLayers), 0.45 * mapExtent);
}

/* ---- Weather map -------------------------------------------------------- */

int CloudVolume_NeedsBake(int body, dvec3 cameraBody, double radius, double extent)
{
	if (!mapValid || body != mapBody)
		return 1;
	double r = dlengthvec3(cameraBody);
	if (r <= 0.0)
		return 0;
	double c = (cameraBody.x * mapCenterB.x + cameraBody.y * mapCenterB.y + cameraBody.z * mapCenterB.z) / r;
	double ang = acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c);
	return ang * radius > extent / 8.0;
}

void CloudVolume_BeginWeatherBake(void)
{
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &savedFBO);
	glGetIntegerv(GL_VIEWPORT, savedViewport);
	glBindFramebuffer(GL_FRAMEBUFFER, mapFBO);
	glViewport(0, 0, CLOUDVOL_MAP_SIZE, CLOUDVOL_MAP_SIZE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(emptyVAO);
}

void CloudVolume_EndWeatherBake(int body, dvec3 mapCenter)
{
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);
	glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	mapValid = 1;
	mapBody = body;
	mapCenterB = mapCenter;
}

void CloudVolume_Invalidate(void)
{
	mapValid = 0;
}

/* ---- Draw --------------------------------------------------------------- */

static dvec3 toView(mat4 v, dvec3 w)
{
	dvec3 r;
	r.x = v.m[0][0] * w.x + v.m[0][1] * w.y + v.m[0][2] * w.z;
	r.y = v.m[1][0] * w.x + v.m[1][1] * w.y + v.m[1][2] * w.z;
	r.z = v.m[2][0] * w.x + v.m[2][1] * w.y + v.m[2][2] * w.z;
	return r;
}

/* World vector -> body-fixed (rotation is body -> world). */
static dvec3 toBody(dmat4 rot, dvec3 w)
{
	dvec3 r;
	r.x = rot.m[0][0] * w.x + rot.m[1][0] * w.y + rot.m[2][0] * w.z;
	r.y = rot.m[0][1] * w.x + rot.m[1][1] * w.y + rot.m[2][1] * w.z;
	r.z = rot.m[0][2] * w.x + rot.m[1][2] * w.y + rot.m[2][2] * w.z;
	return r;
}

static void uniform3d(GLuint p, const char *name, dvec3 v)
{
	glUniform3f(glGetUniformLocation(p, name), (float)v.x, (float)v.y, (float)v.z);
}

static void uniform3f(GLuint p, const char *name, const float v[3])
{
	glUniform3f(glGetUniformLocation(p, name), v[0], v[1], v[2]);
}

/* Halton (2, 3) jitter in [-0.5, 0.5), 8 frames. */
static float halton(int i, int b)
{
	float f = 1.0f, r = 0.0f;
	while (i > 0) {
		f /= (float)b;
		r += f * (float)(i % b);
		i /= b;
	}
	return r;
}

static void drawFullScreen(void)
{
	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
}

/* View -> body-fixed for this frame: column j is view axis j in body
   axes (column-major for glUniformMatrix3fv). */
static void viewToBody(const FrameContext *f, dmat4 rot, float v2b[9])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double acc = 0.0;
			for (int k = 0; k < 3; k++)
				acc += rot.m[k][i] * f->view.m[j][k];
			v2b[j * 3 + i] = (float)acc;
		}
}

/* cloudshape.glsl's uniforms and textures (units 0-3) on program p; *lo,
   *hi: the shell. */
static void setShapeUniforms(GLuint p, const CloudVolumeScene *s, float *lo, float *hi)
{
	int n = s->layerCount > CLOUDVOL_MAX_LAYERS ? CLOUDVOL_MAX_LAYERS : s->layerCount;
	float lb[CLOUDVOL_MAX_LAYERS], lt[CLOUDVOL_MAX_LAYERS], ld[CLOUDVOL_MAX_LAYERS], lc[CLOUDVOL_MAX_LAYERS];
	float ls[CLOUDVOL_MAX_LAYERS], lsw[CLOUDVOL_MAX_LAYERS], lsh[CLOUDVOL_MAX_LAYERS], le[CLOUDVOL_MAX_LAYERS];
	float lan[CLOUDVOL_MAX_LAYERS], lsf[CLOUDVOL_MAX_LAYERS], lri[CLOUDVOL_MAX_LAYERS], lst[CLOUDVOL_MAX_LAYERS];
	float ltint[CLOUDVOL_MAX_LAYERS * 3];
	*lo = 1e30f;
	*hi = -1e30f;
	for (int i = 0; i < CLOUDVOL_MAX_LAYERS; i++) {
		const CloudVolumeLayer *L = &s->layers[i];
		int on = i < n;
		lb[i] = L->base;
		lt[i] = on ? L->top : L->base;
		ld[i] = on ? L->density * s->fade : 0.0f;
		lc[i] = L->coverage;
		ls[i] = L->scale > 1.0f ? L->scale : 1.0f;
		lsw[i] = L->sweep > 0.1f ? L->sweep : 0.1f;
		lsh[i] = L->shear;
		le[i] = L->erosion;
		lan[i] = L->anvil;
		lsf[i] = L->shelf;
		lri[i] = L->ridges;
		lst[i] = L->stretch > 0.1f ? L->stretch : 1.0f;
		for (int c = 0; c < 3; c++)
			ltint[i * 3 + c] = L->tint[c];
		if (on) {
			*lo = fminf(*lo, L->base);
			*hi = fmaxf(*hi, L->top);
		}
	}
	glUniform1i(glGetUniformLocation(p, "layerCount"), n);
	glUniform1fv(glGetUniformLocation(p, "layerBase"), CLOUDVOL_MAX_LAYERS, lb);
	glUniform1fv(glGetUniformLocation(p, "layerTop"), CLOUDVOL_MAX_LAYERS, lt);
	glUniform1fv(glGetUniformLocation(p, "layerDensity"), CLOUDVOL_MAX_LAYERS, ld);
	glUniform1fv(glGetUniformLocation(p, "layerCoverage"), CLOUDVOL_MAX_LAYERS, lc);
	glUniform1fv(glGetUniformLocation(p, "layerScale"), CLOUDVOL_MAX_LAYERS, ls);
	glUniform1fv(glGetUniformLocation(p, "layerSweep"), CLOUDVOL_MAX_LAYERS, lsw);
	glUniform1fv(glGetUniformLocation(p, "layerShear"), CLOUDVOL_MAX_LAYERS, lsh);
	glUniform1fv(glGetUniformLocation(p, "layerErosion"), CLOUDVOL_MAX_LAYERS, le);
	glUniform1fv(glGetUniformLocation(p, "layerAnvil"), CLOUDVOL_MAX_LAYERS, lan);
	glUniform1fv(glGetUniformLocation(p, "layerShelf"), CLOUDVOL_MAX_LAYERS, lsf);
	glUniform1fv(glGetUniformLocation(p, "layerRidges"), CLOUDVOL_MAX_LAYERS, lri);
	glUniform1fv(glGetUniformLocation(p, "layerStretch"), CLOUDVOL_MAX_LAYERS, lst);
	glUniform3fv(glGetUniformLocation(p, "layerTint"), CLOUDVOL_MAX_LAYERS, ltint);
	glUniform1f(glGetUniformLocation(p, "shellLo"), *lo);
	glUniform1f(glGetUniformLocation(p, "shellHi"), *hi);
	uniform3d(p, "mapEast", s->mapEast);
	uniform3d(p, "mapNorth", s->mapNorth);
	glUniform1f(glGetUniformLocation(p, "mapExtent"), (float)s->mapExtent);
	glUniform1f(glGetUniformLocation(p, "detailScale"), g_params.detailScale);

	glActiveTexture(GL_TEXTURE0 + UNIT_WEATHER);
	glBindTexture(GL_TEXTURE_2D, weatherTex);
	glUniform1i(glGetUniformLocation(p, "weatherMap"), UNIT_WEATHER);
	glActiveTexture(GL_TEXTURE0 + UNIT_COLOR);
	glBindTexture(GL_TEXTURE_2D, colorTex);
	glUniform1i(glGetUniformLocation(p, "colorMap"), UNIT_COLOR);
	glActiveTexture(GL_TEXTURE0 + UNIT_SHAPE);
	glBindTexture(GL_TEXTURE_3D, shapeTex);
	glUniform1i(glGetUniformLocation(p, "shapeNoise"), UNIT_SHAPE);
	glActiveTexture(GL_TEXTURE0 + UNIT_DETAIL);
	glBindTexture(GL_TEXTURE_3D, detailTex);
	glUniform1i(glGetUniformLocation(p, "detailNoise"), UNIT_DETAIL);
}

static int sceneReady(const CloudVolumeScene *s)
{
	return g_params.enabled && s->fade > 0.0f && s->layerCount > 0 && mapValid && mapBody == s->body;
}

void CloudVolume_UpdateShadow(const FrameContext *f, const CloudVolumeScene *s)
{
	shadowValid = 0;
	if (!sceneReady(s) || g_params.shadowExtent <= 0.0f)
		return;
	/* Centred under the camera, in the weather map's plane. */
	dvec3 camB = toBody(s->rotation, dsubvec3(f->cam, s->center));
	dvec3 mapC = dscalarvec3(mapCenterB, s->radius);
	dvec3 camToMap = dsubvec3(camB, mapC);
	shadowCX = camToMap.x * s->mapEast.x + camToMap.y * s->mapEast.y + camToMap.z * s->mapEast.z;
	shadowCY = camToMap.x * s->mapNorth.x + camToMap.y * s->mapNorth.y + camToMap.z * s->mapNorth.z;
	shadowExtentM = g_params.shadowExtent;
	shadowSunB = toBody(s->rotation, s->sunDir);
	shadowRotation = s->rotation;
	shadowBodyCenter = s->center;
	shadowMapCenter = mapC;
	shadowEast = s->mapEast;
	shadowNorth = s->mapNorth;

	GLint fbo, vp[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
	glViewport(0, 0, CLOUDVOL_SHADOW_SIZE, CLOUDVOL_SHADOW_SIZE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);

	GLuint p = shadowProgram;
	glUseProgram(p);
	setShapeUniforms(p, s, &shadowLo, &shadowHi);
	uniform3d(p, "mapCenterB", mapC);
	glUniform2f(glGetUniformLocation(p, "shadowCenter"), (float)shadowCX, (float)shadowCY);
	glUniform1f(glGetUniformLocation(p, "shadowExtent"), (float)shadowExtentM);
	glUniform1f(glGetUniformLocation(p, "shadowSize"), (float)CLOUDVOL_SHADOW_SIZE);
	uniform3d(p, "sunB", shadowSunB);
	glUniform1f(glGetUniformLocation(p, "radius"), (float)s->radius);
	glUniform1i(glGetUniformLocation(p, "steps"), SHADOW_STEPS);
	drawFullScreen();

	for (int u = UNIT_DETAIL; u >= 0; u--) {
		glActiveTexture(GL_TEXTURE0 + u);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindTexture(GL_TEXTURE_3D, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (depth)
		glEnable(GL_DEPTH_TEST);
	if (blend)
		glEnable(GL_BLEND);
	if (cull)
		glEnable(GL_CULL_FACE);
	shadowValid = 1;
}

void CloudVolume_BindShadow(GLuint p, int unit, const FrameContext *f)
{
	glUniform1i(glGetUniformLocation(p, "csOn"), shadowValid);
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, shadowValid ? shadowTex : 0);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(p, "cloudShadowMap"), unit);
	if (!shadowValid)
		return;
	float v2b[9];
	viewToBody(f, shadowRotation, v2b);
	glUniformMatrix3fv(glGetUniformLocation(p, "csViewToBody"), 1, GL_FALSE, v2b);
	dvec3 camB = toBody(shadowRotation, dsubvec3(f->cam, shadowBodyCenter));
	uniform3d(p, "csCamToMap", dsubvec3(camB, shadowMapCenter));
	uniform3d(p, "csEast", shadowEast);
	uniform3d(p, "csNorth", shadowNorth);
	glUniform2f(glGetUniformLocation(p, "csCenter"), (float)shadowCX, (float)shadowCY);
	glUniform1f(glGetUniformLocation(p, "csExtent"), (float)shadowExtentM);
	glUniform1f(glGetUniformLocation(p, "csLo"), shadowLo);
	glUniform1f(glGetUniformLocation(p, "csHi"), shadowHi);
	uniform3d(p, "csSun", shadowSunB);
}

void CloudVolume_Draw(const FrameContext *f, const CloudVolumeScene *s)
{
	if (!sceneReady(s)) {
		prevValid = 0;
		return;
	}
	frameCount++;

	GLint fbo, vp[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	int div = g_params.resDivisor < 1 ? 1 : g_params.resDivisor > 8 ? 8 : g_params.resDivisor;
	int w = (vp[2] + div - 1) / div, h = (vp[3] + div - 1) / div;
	if (ensureLowTargets(w, h))
		prevValid = 0;

	/* The camera and view in the body's frame. */
	dvec3 camB = toBody(s->rotation, dsubvec3(f->cam, s->center));
	double camR = dlengthvec3(camB);
	double camAlt = camR - s->radius;
	dvec3 upW = dsubvec3(f->cam, s->center);
	upW = dscalarvec3(upW, 1.0 / dlengthvec3(upW));
	dvec3 upV = toView(f->view, upW);
	dvec3 sunV = toView(f->view, s->sunDir);
	float v2b[9];
	viewToBody(f, s->rotation, v2b);
	dvec3 camToMap = dsubvec3(camB, dscalarvec3(mapCenterB, s->radius));

	/* History: same body, and the camera has not jumped. */
	dvec3 camDelta = dsubvec3(camB, prevCamB);
	int useHistory = prevValid && prevBody == s->body && dlengthvec3(camDelta) < 0.25 * g_params.maxDistance;

	/* 1. March into the low-resolution target. */
	glBindFramebuffer(GL_FRAMEBUFFER, marchFBO);
	glViewport(0, 0, w, h);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_CULL_FACE);

	GLuint p = marchProgram;
	glUseProgram(p);
	glUniformMatrix4fv(glGetUniformLocation(p, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	glUniform2f(glGetUniformLocation(p, "lowSize"), (float)w, (float)h);
	glUniform2f(glGetUniformLocation(p, "fullSize"), (float)vp[2], (float)vp[3]);
	int ji = (int)(frameCount % 8u) + 1;
	glUniform2f(glGetUniformLocation(p, "jitter"), halton(ji, 2) - 0.5f, halton(ji, 3) - 0.5f);
	glUniform1f(glGetUniformLocation(p, "frameJitter"), (float)fmod(frameCount * 0.61803398875, 1.0));
	glUniformMatrix3fv(glGetUniformLocation(p, "viewToBody"), 1, GL_FALSE, v2b);
	uniform3d(p, "camUp", upV);
	uniform3d(p, "sunDir", sunV);
	glUniform1f(glGetUniformLocation(p, "radius"), (float)s->radius);
	glUniform1f(glGetUniformLocation(p, "camAlt"), (float)camAlt);
	uniform3d(p, "camToMap", camToMap);
	float lo, hi;
	setShapeUniforms(p, s, &lo, &hi);
	CloudVolume_BindShadow(p, UNIT_SHADOW, f);

	/* Flashes, camera-relative in view axes. */
	int nf = s->flashCount < 0 ? 0 : s->flashCount > CLOUDVOL_MAX_FLASHES ? CLOUDVOL_MAX_FLASHES : s->flashCount;
	float fp[CLOUDVOL_MAX_FLASHES * 3], fc[CLOUDVOL_MAX_FLASHES * 3], fr[CLOUDVOL_MAX_FLASHES];
	memset(fp, 0, sizeof fp);
	memset(fc, 0, sizeof fc);
	for (int i = 0; i < CLOUDVOL_MAX_FLASHES; i++) {
		fr[i] = 1.0f;
		if (i >= nf)
			continue;
		const CloudVolumeFlash *F = &s->flashes[i];
		dvec3 v = toView(f->view, dsubvec3(F->position, f->cam));
		fp[i * 3 + 0] = (float)v.x;
		fp[i * 3 + 1] = (float)v.y;
		fp[i * 3 + 2] = (float)v.z;
		for (int c = 0; c < 3; c++)
			fc[i * 3 + c] = F->color[c];
		fr[i] = F->radius > 1.0f ? F->radius : 1.0f;
	}
	glUniform1i(glGetUniformLocation(p, "flashCount"), nf);
	glUniform3fv(glGetUniformLocation(p, "flashPos"), CLOUDVOL_MAX_FLASHES, fp);
	glUniform3fv(glGetUniformLocation(p, "flashColor"), CLOUDVOL_MAX_FLASHES, fc);
	glUniform1fv(glGetUniformLocation(p, "flashRadius"), CLOUDVOL_MAX_FLASHES, fr);

	uniform3f(p, "airAlbedo", s->airAlbedo);
	glUniform1f(glGetUniformLocation(p, "airAmbient"), s->airAmbient);
	glUniform1f(glGetUniformLocation(p, "airNight"), s->airNight);
	glUniform1f(glGetUniformLocation(p, "airForward"), s->airForward);
	glUniform1f(glGetUniformLocation(p, "airColumn"), (float)s->airColumn);
	glUniform1f(glGetUniformLocation(p, "airScaleHeight"), (float)s->airScaleHeight);
	uniform3f(p, "terminator", s->terminator);
	glUniform1f(glGetUniformLocation(p, "wrap"), s->wrap);
	uniform3f(p, "skyColor", s->skyColor);
	glUniform1f(glGetUniformLocation(p, "skyGain"), s->skyGain);
	uniform3f(p, "groundColor", s->groundColor);
	glUniform1f(glGetUniformLocation(p, "groundGain"), s->groundGain);
	glUniform1f(glGetUniformLocation(p, "exposure"), s->exposure);

	glUniform1i(glGetUniformLocation(p, "viewSteps"), g_params.viewSteps);
	glUniform1i(glGetUniformLocation(p, "lightSteps"), g_params.lightSteps);
	glUniform1f(glGetUniformLocation(p, "maxDistance"), (float)CloudVolume_Reach(camAlt - lo, s->mapExtent));
	glUniform1f(glGetUniformLocation(p, "multiScatter"), g_params.multiScatter);
	glUniform1f(glGetUniformLocation(p, "powder"), g_params.powder);
	glUniform1f(glGetUniformLocation(p, "ambient"), g_params.ambient);

	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneDepthTex);
	glUniform1i(glGetUniformLocation(p, "sceneDepth"), UNIT_DEPTH);
	glUniform1i(glGetUniformLocation(p, "sceneSamples"), sceneDepthTex ? sceneDepthSamples : 0);
	glUniform1f(glGetUniformLocation(p, "depthNear"), sceneNear);
	drawFullScreen();
	glActiveTexture(GL_TEXTURE0 + UNIT_DEPTH);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

	/* 2. Accumulate: this frame over the reprojected history. */
	int cur = histIndex ^ 1;
	glBindFramebuffer(GL_FRAMEBUFFER, histFBO[cur]);
	p = resolveProgram;
	glUseProgram(p);
	glUniformMatrix4fv(glGetUniformLocation(p, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	glUniform2f(glGetUniformLocation(p, "lowSize"), (float)w, (float)h);
	glUniformMatrix3fv(glGetUniformLocation(p, "viewToBody"), 1, GL_FALSE, v2b);
	/* Body -> previous view: the transpose of last frame's view -> body. */
	glUniformMatrix3fv(glGetUniformLocation(p, "bodyToPrevView"), 1, GL_TRUE, prevViewToBody);
	uniform3d(p, "camDelta", camDelta);
	glUniform1f(glGetUniformLocation(p, "historyWeight"), useHistory ? g_params.history : 0.0f);
	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, marchColor);
	glUniform1i(glGetUniformLocation(p, "curColor"), 0);
	glActiveTexture(GL_TEXTURE0 + 1);
	glBindTexture(GL_TEXTURE_2D, marchDepth);
	glUniform1i(glGetUniformLocation(p, "curDepth"), 1);
	glActiveTexture(GL_TEXTURE0 + 2);
	glBindTexture(GL_TEXTURE_2D, histColor[histIndex]);
	glUniform1i(glGetUniformLocation(p, "histColor"), 2);
	glActiveTexture(GL_TEXTURE0 + 3);
	glBindTexture(GL_TEXTURE_2D, histDepth[histIndex]);
	glUniform1i(glGetUniformLocation(p, "histDepth"), 3);
	drawFullScreen();
	histIndex = cur;

	/* 3. Over the scene, with depth where the clouds are. */
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	p = compositeProgram;
	glUseProgram(p);
	glUniformMatrix4fv(glGetUniformLocation(p, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	glUniform2f(glGetUniformLocation(p, "fullSize"), (float)vp[2], (float)vp[3]);
	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, histColor[cur]);
	glUniform1i(glGetUniformLocation(p, "cloudColor"), 0);
	glActiveTexture(GL_TEXTURE0 + 1);
	glBindTexture(GL_TEXTURE_2D, histDepth[cur]);
	glUniform1i(glGetUniformLocation(p, "cloudDepth"), 1);
	/* Premultiplied, behind anything nearer (the march already stopped
	   at the scene's depth; this keeps the full-resolution edges); alpha
	   keeps the slab's cloud cover. */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GREATER);   /* reversed-Z */
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	drawFullScreen();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	if (cull)
		glEnable(GL_CULL_FACE);
	for (int u = UNIT_SHADOW; u >= 0; u--) {
		glActiveTexture(GL_TEXTURE0 + u);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindTexture(GL_TEXTURE_3D, 0);
	}

	prevValid = 1;
	prevBody = s->body;
	prevCamB = camB;
	memcpy(prevViewToBody, v2b, sizeof v2b);
}

/* ---- Debug panel -------------------------------------------------------- */

void CloudVolume_DebugPanel(struct nk_context *ctx)
{
	DebugGUI_Group(ctx, "Volume (shared)");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Volumetric clouds", &g_params.enabled);
	DebugGUI_SliderInt(ctx, "Resolution 1/n", &g_params.resDivisor, 1, 4);
	DebugGUI_SliderInt(ctx, "View steps", &g_params.viewSteps, 16, 256);
	DebugGUI_SliderInt(ctx, "Light steps", &g_params.lightSteps, 0, 12);
	DebugGUI_Slider(ctx, "Max distance (m)", &g_params.maxDistance, 20.0e3f, 1.5e6f, 1.0e3f, "%.0f");
	DebugGUI_Slider(ctx, "History", &g_params.history, 0.0f, 0.98f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Detail scale", &g_params.detailScale, 0.02f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Multiple scattering", &g_params.multiScatter, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Powder", &g_params.powder, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Skylight", &g_params.ambient, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Shadow map width (m)", &g_params.shadowExtent, 0.0f, 1.0e6f, 1.0e3f, "%.0f");
}
