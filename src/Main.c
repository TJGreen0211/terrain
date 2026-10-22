#include "Main.h"
#include "Sun.h"
#include "Planets.h"
#include "SolarSystem.h"
#include "Skybox.h"
#include "Nebula.h"
#include "Atmosphere.h"
#include "Clouds.h"
#include "LoadingScreen.h"
#include "GUI.h"
#include "DebugGUI.h"
#include "TerrainGen.h"
#include "TerrainDetail.h"
#include "Shadows.h"
#include "Rings.h"
#include "WorldConfig.h"
#include "Bodies.h"
#include "Profiler.h"
#include "Bench.h"
/* stb_image_write lives here; Bench.c uses it for its PNG captures. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stdio.h>
//#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <pthread.h>

/* World units are metres; every scale constant (camera near plane, shadow
   map size, body sizes and orbits, ...) is in WorldConfig.h. The main pass
   uses an infinite reversed-Z projection into a float depth buffer (see the
   scene target below), whose relative precision is the same at every
   distance, so the near plane can sit at surface scale while the sun and
   moon stay in view. */
#define CAMERA_NEAR ((float)CAMERA_NEAR_M)
GLuint depthMap;
/* Per-frame extras for the quadtree body passes (set once per frame, read by
   setBodyUniforms / bindShadowMap). lightSpace maps CAMERA-RELATIVE
   positions to the shadow map's clip space; shadowTexel is the world size of
   one shadow texel at the planet (scales the receiver bias). */
mat4 g_lightSpaceMatrix;
float g_shadowTexel = 1.0f;
float g_waveParams[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // oceanOffset, waveAmp, normalStrength, wave scale, choppiness
/* Baked planet height map (R32F cube map + mips, TerrainGen.c), sampled by
   quad.frag/water.frag instead of evaluating simplex noise per pixel.

   The height field lives on the unit sphere: a screen pixel at surface
   distance dist covers dist*thetaPixel world units, i.e. dist*thetaPixel/r
   radians on the sphere, while one face-centre texel covers
   TerrainGen_MapTexelRad() radians. With thetaPixel = 45deg/H that gives
   lod = log2(dist * LOD_K/(r*H)) plus the grazing and face-corner
   corrections in the shaders (planetmap.glsl). */
GLuint planetHeightTex;
#define LOD_K_NUMER ((float)(0.7853982 / TerrainGen_MapTexelRad()))
/* Geometric relief (quad.vert / depth.vert displace the unit sphere by it)
   is g_planetStyle.landRelief / oceanRelief (TerrainGen.h). Sea level is
   radius 1.0 for both the planet and the water shell; the squared shelf
   profile keeps vertices next to the coast near sea level, so
   coast-straddling triangles barely sag. */
struct sphere planet;
struct ring planetRing;
struct quadCube qc, qc2;

/* Camera-relative rendering (floating origin, ported from OpenGLSolar):
   world positions stay in double on the CPU, the camera is subtracted in
   double, and only the small difference is downcast to the float matrices
   the GPU sees. The view matrix is rotation only, so the camera sits at the
   origin of every shader space. g_cam is latched once per frame, before any
   draw, so every pass of a frame shares one origin. */
static dvec3 g_cam;

static vec3 toCamRel(dvec3 world) {
	return dvec3tovec3(dsubvec3(world, g_cam));
}

static mat4 camRelTranslate(dvec3 world) {
	return translatevec3(toCamRel(world));
}

/* The planet surface program. depthPrepass = 1 builds the depth-only
   variant (surface depth with the seafloor pushed behind the water); the
   colour variant never writes depth, so the GPU keeps early-Z for it (see
   quad.frag). */
GLuint initQuadShader(int depthPrepass) {
	GLuint shader;
	/* Per-pixel terrain detail octaves (detail.glsl) are compiled in or out:
	   even a skipped loop costs the large planet shader registers. */
	char defines[128];
	snprintf(defines, sizeof defines, "%s#define DETAIL_PIXEL_OCTAVES %d\n",
	         depthPrepass ? "#define DEPTH_PREPASS\n" : "", TERRAIN_DETAIL_PIXEL_OCTAVES);
	createShaderDefines(&shader, "shaders/quad.vert", "shaders/quad.frag", defines);
	return shader;
}

GLuint initPlanetShader() {
	GLuint shader;
	GLuint vertShader = LoadShader("shaders/planet.vert", GL_VERTEX_SHADER);
	GLuint tcshShader = LoadShader("shaders/planet.tcsh", GL_TESS_CONTROL_SHADER);
	GLuint teshShader = LoadShader("shaders/planet.tesh", GL_TESS_EVALUATION_SHADER);
	//GLuint geomShader = LoadShader("shaders/tess.geom", GL_GEOMETRY_SHADER);
	GLuint fragShader = LoadShader("shaders/planet.frag", GL_FRAGMENT_SHADER);
	shader = glCreateProgram();
	glAttachShader(shader, vertShader);
	glAttachShader(shader, tcshShader);
	glAttachShader(shader, teshShader);
	//glAttachShader(shader, geomShader);
	glAttachShader(shader, fragShader);
	glLinkProgram(shader);

	return shader;
}

GLuint initInstanceShader() {
	GLuint shader;
	createShader(&shader, "shaders/instance.vert",
		"shaders/instance.frag");
	return shader;
}

/* Camera sliders (bottom-left, blue = movement speed on top, orange =
   scroll speed below), ported from OpenGLSolar. Log-mapped so the low end
   stays usable: t in [0,1] -> min*(max/min)^t. The ranges (WorldConfig.h)
   keep the camera defaults at the same relative spot OpenGLSolar's do. */
#define UI_SLIDER_MOVEMENT 0
#define UI_SLIDER_SCROLL   1
#define UI_SLIDER_COUNT    2

#define CAMERA_MOVEMENT_MIN ((float)CAMERA_MOVE_SPEED_MIN)
#define CAMERA_MOVEMENT_MAX ((float)CAMERA_MOVE_SPEED_MAX)
#define CAMERA_SCROLL_MIN ((float)CAMERA_SCROLL_STEP_MIN)
#define CAMERA_SCROLL_MAX ((float)CAMERA_SCROLL_STEP_MAX)

static float cameraSliderValue(float minV, float maxV, float t)
{
	return minV * powf(maxV / minV, t);
}

static float cameraSliderInitT(float minV, float maxV, float value)
{
	return logf(value / minV) / logf(maxV / minV);
}

void onCameraSliderChanged(int id, float t)
{
	if (id == UI_SLIDER_MOVEMENT)
		MovementSpeed = cameraSliderValue(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, t);
	else if (id == UI_SLIDER_SCROLL)
		CameraScrollSpeed = cameraSliderValue(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, t);
}

/* Accent colours double as labels: blue = movement, orange = scroll. */
static const float sliderAccentMovement[3] = { 0.45f, 0.65f, 1.0f };
static const float sliderAccentScroll[3] = { 1.0f, 0.55f, 0.30f };

void uiSetupCameraSliders()
{
	uiSliderSetChangedFn(onCameraSliderChanged);
	/* t0 puts each handle at the current camera default. */
	uiAddSlider(UI_SLIDER_MOVEMENT,
		cameraSliderInitT(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, MovementSpeed),
		sliderAccentMovement);
	uiAddSlider(UI_SLIDER_SCROLL,
		cameraSliderInitT(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, CameraScrollSpeed),
		sliderAccentScroll);
}

GLuint initAtmosphereShader() {
	GLuint shader;
	createShader(&shader, "shaders/atmosphere.vert",
		"shaders/atmosphere.frag");
	return shader;
}

GLuint initLightingShader() {
	GLuint shader;
	createShader(&shader, "shaders/light.vert",
		"shaders/light.frag");
	return shader;
}

GLuint initWaterShader() {
	GLuint shader;
	createShader(&shader, "shaders/water.vert",
		"shaders/water.frag");
	return shader;
}

GLuint initWTestShader() {
	GLuint shader;
	createShader(&shader, "shaders/wtest.vert",
		"shaders/wtest.frag");
	return shader;
}

GLuint initDepthShader() {
	GLuint shader;
	createShader(&shader, "shaders/depth.vert",
		"shaders/depth.frag");

	return shader;
}

GLuint initTessShader() {
	GLuint shader;
	GLuint vertShader = LoadShader("shaders/tess.vert", GL_VERTEX_SHADER);
	GLuint tcshShader = LoadShader("shaders/tess.tcsh", GL_TESS_CONTROL_SHADER);
	GLuint teshShader = LoadShader("shaders/tess.tesh", GL_TESS_EVALUATION_SHADER);
	//GLuint geomShader = LoadShader("shaders/tess.geom", GL_GEOMETRY_SHADER);
	GLuint fragShader = LoadShader("shaders/tess.frag", GL_FRAGMENT_SHADER);
	shader = glCreateProgram();
	glAttachShader(shader, vertShader);
	glAttachShader(shader, tcshShader);
	glAttachShader(shader, teshShader);
	//glAttachShader(shader, geomShader);
	glAttachShader(shader, fragShader);
	glLinkProgram(shader);

	return shader;
}

GLuint initDepthbuffer() {
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	/* 32-bit float depth, sampled through sampler2DShadow: LINEAR filtering
	   with a compare mode makes every texture() a bilinear 2x2 PCF. Outside
	   the map the border depth is the far plane, i.e. lit. */
	glGenTextures(1, &depthMap);
	glBindTexture(GL_TEXTURE_2D, depthMap);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthMap, 0);
	GLuint status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	  if(status != GL_FRAMEBUFFER_COMPLETE)
		  printf("GL_FRAMEBUFFER_COMPLETE failed, CANNOT use FBO\n");

	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return fbo;
}

GLuint initQuadCube(int divisions) {
	GLuint vao;
	createCube(divisions, &qc);
	vec2 *texCoords = malloc(qc.vertexNumber*sizeof(vec2));
	vec3 *vna = malloc(qc.vertexNumber*sizeof(vec3));
	vec3 *tangent = malloc(qc.vertexNumber*sizeof(vec3));
	*tangent = *generateTangents(qc.vertexNumber, qc.points, tangent);

	for(int i = 0; i < qc.vertexNumber; i++) {
    	texCoords[i].x = (atan2(qc.points[i].y, qc.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(qc.points[i].z) / 3.1415926 + 0.5;
    }
    *vna = *generateSmoothNormals(vna, qc.points, qc.normals, qc.vertexNumber);
    //vao = initBuffers(qc.points, qc.size, vna, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);
    vao = initBufferTangents(qc.points, qc.size, vna, qc.nsize, tangent, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);

    return vao;
}

GLuint initObjectBuffer(char *path, obj *object) {
	GLuint vao;
	*object = ObjLoadModel(path);
	vec3 *vna = malloc(object->vertexNumber*sizeof(vec3));
	vec2 *texCoords = malloc(object->vertexNumber*sizeof(vec2));
	*vna = *generateSmoothNormals(vna, object->points, object->normals, object->vertexNumber);

	for(int i = 0; i < object->vertexNumber; i++) {
    	texCoords[i].x = 1.0;
    	texCoords[i].y = 0.0;
    }
    vao = initBuffers(object->points, object->size, object->normals, object->nsize, texCoords, sizeof(texCoords[0])*object->vertexNumber);

	return vao;
}

GLuint initSphere() {
	planet = tetrahedron(5, &planet);
	GLuint vao;
    vec3 *vna = malloc(planet.vertexNumber*sizeof(vec3));
    vec2 *texCoords = malloc(planet.vertexNumber*sizeof(vec2));
    *vna = *generateSmoothNormals(vna, planet.points, planet.normals, planet.vertexNumber);

    for(int i = 0; i < planet.vertexNumber; i++) {
    	texCoords[i].x = (atan2(planet.points[i].y, planet.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(planet.points[i].z) / 3.1415926 + 0.5;
    }

	vao = initBuffers(planet.points, planet.size, planet.normals, planet.nsize, texCoords, sizeof(texCoords[0])*planet.vertexNumber);
	return vao;
}

GLuint initRing() {
	GLuint vao;
	planetRing = createRing(1, 1.5, 2.0);
	vao = initBuffers(planetRing.points, planetRing.size, planetRing.normals, planetRing.nsize, planetRing.texCoords, planetRing.texsize);
	return vao;
}

vec4 getPositionModelspace(mat4 position, mat4 model) {
	mat4 mvTranspose = transposemat4(multiplymat4(position, model));
	vec4 inverseM = {-mvTranspose.m[3][0], -mvTranspose.m[3][1], -mvTranspose.m[3][2], -mvTranspose.m[3][3]};
	vec4 positionModelspace = multiplymat4vec4(mvTranspose, inverseM);

	return positionModelspace;
}

/* Light view for the shadow map, built for CAMERA-RELATIVE positions:
   a right-handed look-at from the sun toward the planet centre. The basis
   comes from the world-space direction (camera independent) and the
   translation is dotted against the camera-relative eye in double, so the
   matrix stays exact however far the camera is from the world origin. */
static mat4 getLightViewMatrix(dvec3 lightWorld, dvec3 targetWorld) {
	double f[3] = { targetWorld.x - lightWorld.x, targetWorld.y - lightWorld.y, targetWorld.z - lightWorld.z };
	double fl = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
	f[0] /= fl; f[1] /= fl; f[2] /= fl;
	/* Any up vector not parallel to the light direction works. */
	double up[3] = { 0.0, 1.0, 0.0 };
	if (fabs(f[1]) > 0.99) { up[0] = 1.0; up[1] = 0.0; }
	double r[3] = { f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0] };
	double rl = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
	r[0] /= rl; r[1] /= rl; r[2] /= rl;
	double u[3] = { r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2], r[0]*f[1] - r[1]*f[0] };
	dvec3 e = dsubvec3(lightWorld, g_cam);

	mat4 v = scale(1.0);   /* identity */
	for (int k = 0; k < 3; k++) {
		v.m[0][k] = (float)r[k];
		v.m[1][k] = (float)u[k];
		v.m[2][k] = (float)-f[k];
	}
	v.m[0][3] = (float)-(r[0]*e.x + r[1]*e.y + r[2]*e.z);
	v.m[1][3] = (float)-(u[0]*e.x + u[1]*e.y + u[2]*e.z);
	v.m[2][3] = (float)(f[0]*e.x + f[1]*e.y + f[2]*e.z);
	return v;
}

/* Shadow map binding shared by every receiver program (planet, water, moon,
   sun marker): sampler unit, light-space matrix and texel size. */
static void bindShadowMap(GLuint shader, int unit) {
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, depthMap);
	glUniform1i(glGetUniformLocation(shader, "depthMap"), unit);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &g_lightSpaceMatrix.m[0][0]);
	glUniform1f(glGetUniformLocation(shader, "shadowTexel"), g_shadowTexel);
	glActiveTexture(GL_TEXTURE0);
	/* Cascades (shadow.glsl; programs without them ignore the uniforms).
	   Unit 13: 0-12 are taken (atmosphere depth uses 12). */
	Shadows_BindReceiver(shader, 13);
}

/* ---- Main-pass depth: reversed-Z into a float depth buffer ----
   The window's default depth buffer is 24-bit fixed point, which reversed-Z
   cannot help, so the scene renders into its own multisampled target (RGBA8
   + DEPTH32F) that is resolved into the window at the end of the frame.
   The depth is a multisample texture so the atmosphere dome can read it
   (Atmosphere_SetSceneDepth) to end its rays at the visible surface.
   Clip control is per pass: the main pass uses GL_ZERO_TO_ONE with
   GL_GREATER and a depth clear of 0; everything else (shadow map, bakes,
   GPU wave passes, UI) keeps the standard convention. */
#define SCENE_SAMPLES RENDER_MSAA_SAMPLES   /* WorldConfig.h */
static GLuint g_sceneFBO, g_sceneColorRB, g_sceneDepthTex;
static int g_sceneW, g_sceneH;

static void ensureSceneTarget(int w, int h) {
	if (g_sceneFBO && w == g_sceneW && h == g_sceneH)
		return;
	if (!g_sceneFBO) {
		glGenFramebuffers(1, &g_sceneFBO);
		glGenRenderbuffers(1, &g_sceneColorRB);
		glGenTextures(1, &g_sceneDepthTex);
	}
	g_sceneW = w;
	g_sceneH = h;
	glBindRenderbuffer(GL_RENDERBUFFER, g_sceneColorRB);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, SCENE_SAMPLES, GL_RGBA8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	/* Fixed sample locations: required to mix with the colour renderbuffer. */
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, g_sceneDepthTex);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, SCENE_SAMPLES, GL_DEPTH_COMPONENT32F, w, h, GL_TRUE);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFBO);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_sceneColorRB);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, g_sceneDepthTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[main] scene framebuffer incomplete\n");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	Atmosphere_SetSceneDepth(g_sceneDepthTex, SCENE_SAMPLES, CAMERA_NEAR);
	Nebula_SetSceneDepth(g_sceneDepthTex, SCENE_SAMPLES, CAMERA_NEAR);
}

/* Switches clip control, depth test and depth clear value between the main
   pass (reversed) and everything else (standard). Requires
   GL_ARB_clip_control (checked at startup). */
static void setReversedDepth(int on) {
	glClipControl(GL_LOWER_LEFT, on ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
	glDepthFunc(on ? GL_GREATER : GL_LESS);
	glClearDepth(on ? 0.0 : 1.0);
}

/* Main camera projection for every main-pass draw (infinite reversed-Z). */
static mat4 cameraProjection(void) {
	return perspectiveReversedInf((float)CAMERA_FOV_Y_DEG, getWindowWidth()/getWindowHeight(), CAMERA_NEAR);
}

/* Main camera projection * view, matching initMVP/setBodyUniforms; the
   quadtree walk frustum-culls its nodes against it (with the infinite
   projection its far plane never culls). */
static mat4 getViewProjection(void) {
	return multiplymat4(cameraProjection(), getViewMatrix());
}

void initMVP(int shader, mat4 m, mat4 v) {
	mat4 p = cameraProjection();
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &p.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &v.m[0][0]);
}

/* water.vert's geometric wave scale: the FFT field's rms (body radii)
   mapped to OCEAN_SWELL_RMS_M of real swell on this body. Each cascade's
   heights are further scaled by waveCascadeScale (Water.c), with its
   patches, so slopes (and look) stay the same, only larger. */
static double waveGeomScale(double radius)
{
	return OCEAN_SWELL_RMS_M / (OCEAN_FFT_RMS * radius);
}

/* Water shell bound, in rms of the displaced swell: the sea state can
   double it (water.vert's calm clamp) and peaks stay within ~4 rms.
   Choppy displacement moves vertices sideways by the same scale. */
#define WAVE_BOUND_RMS 8.0
/* Shore calm, in rms of each cascade's drawn height: its waves fade out
   from this water depth to the coastline. */
#define WAVE_SHORE_RMS 4.0

/* How far (body radii) the displaced water shell can stray from sea level:
   the culling bound of the water walk, and the clearance quad.frag keeps
   the seafloor under it. The largest cascade scale bounds the sum (the
   finest stays at 1). */
static double waveExcursion(double radius)
{
	return WAVE_BOUND_RMS * OCEAN_FFT_RMS * waveGeomScale(radius)
	     * fmax(g_waveParams[3], 1.0)
	     * g_waveParams[1] * (1.0 + g_waveParams[4]);
}

/* quad.vert's relief profile (body radii) for a raw height h. Monotonic in
   h, so a height range maps straight to a relief range. */
static double reliefOf(double h)
{
	double sea = g_planetGen.seaLevel;
	double s = fmin(fmax((sea - h) / fmax(g_planetGen.oceanDepth, 1e-4), 0.0), 1.0);
	return g_planetStyle.landRelief * fmax(h - sea, 0.0) / fmax(1.0 - sea, 1e-3)
	     - g_planetStyle.oceanRelief * s * s;
}

/* quad.vert / depth.vert's lodCellK: the height-map mip a vertex at
   distance d samples is log2(d * lodCellK), the mip of the widest leaf the
   main walk can have there (half-width d / (2 K R) cube units, K from
   QT_TARGET_TRIANGLE_PX; a face-centre node is ~2 R x its half-width
   across). A function of distance, not of the patch's level, so the
   surface has no height steps where patch levels meet (they showed through
   the atmosphere's depth-clipped haze), and it is never finer than the old
   per-level mip. */
static double lodCellKFor(double radius)
{
	return 1.0 / (TerrainGen_MapTexelRad() * QuadTree_SplitK(QT_TARGET_TRIANGLE_PX) * radius);
}

/* View distances the terrain can be seen over, for fitting the shadow
   cascades. nearest: the camera's height above the highest terrain within
   its horizon (no ground is closer); farthest: the distance to the horizon
   plus as far as that terrain's peaks can peek over it. */
static void terrainViewRange(const Body *planet, double *nearest, double *farthest)
{
	dvec3 rel = dsubvec3(g_cam, planet->position);
	double d = dlengthvec3(rel), R = planet->radius;
	float gmin, gmax;
	TerrainGen_GlobalRange(&gmin, &gmax);
	double topGlobal = R * (1.0 + reliefOf(gmax));
	double cap = d > topGlobal ? acos(R / d) + acos(R / topGlobal) : M_PI;
	/* Body-local nadir: rotation is body -> world, so its transpose. */
	double w[3] = { rel.x / d, rel.y / d, rel.z / d }, dl[3];
	const double (*m)[4] = planet->rotation.m;
	for (int j = 0; j < 3; j++)
		dl[j] = m[0][j] * w[0] + m[1][j] * w[1] + m[2][j] * w[2];
	float lo, hi;
	TerrainGen_HeightRange(dl, fmin(cap, M_PI), 6.0, &lo, &hi);
	double top = R * (1.0 + fmax(reliefOf(hi), 0.0));
	*nearest = fmax(d - top, 0.0);
	*farthest = sqrt(fmax(d * d - R * R, 0.0)) + sqrt(fmax(top * top - R * R, 0.0));
}

/* Radius (m) of the highest terrain within capAngle of a world direction,
   detail noise included: Bench.c's 'poff ground'. */
double Earth_GroundRadius(dvec3 worldDir, double capAngle)
{
	const Body *earth = &g_bodies[BODY_EARTH];
	double w[3] = { worldDir.x, worldDir.y, worldDir.z }, dl[3];
	const double (*m)[4] = earth->rotation.m;
	for (int j = 0; j < 3; j++)
		dl[j] = m[0][j] * w[0] + m[1][j] * w[1] + m[2][j] * w[2];
	float lo, hi;
	TerrainGen_HeightRange(dl, capAngle, -HUGE_VAL, &lo, &hi);   /* finest mip */
	return earth->radius * (1.0 + fmax(reliefOf(hi), 0.0)) + TerrainDetail_Amplitude(earth->radius);
}

/* QuadTree_ReliefFn for the active planet's land + seafloor (quad.vert, depth.vert):
   the terrain the shaders can place over the cap, same mip selection (the
   coarsest any vertex there uses, at maxDist: the pyramid range at a mip
   bounds every finer sample). */
static void terrainReliefBounds(const double dir[3], double capAngle, double cell,
                                double maxDist, double *lo, double *hi)
{
	(void)cell;
	double R = g_bodies[SolarSystem_ActiveBody()].radius;
	double lodK = LOD_K_NUMER / (R * getWindowHeight());
	/* Unclamped: HeightRange adds the face-corner bias, then clamps. */
	double lodCell = log2(fmax(maxDist * lodCellKFor(R), 1e-30));
	double lodPix = log2(fmax(maxDist * lodK, 1e-30));
	double lod = fmin(fmax(lodCell, lodPix), TerrainGen_MapMaxLod());
	float hmin, hmax;
	TerrainGen_HeightRange(dir, capAngle, lod, &hmin, &hmax);
	*lo = reliefOf(hmin);
	*hi = reliefOf(hmax);
}

/* 1 if every height a fragment shader can sample over the cap is above
   sea level, i.e. no pixel there is sea: water.frag discards all of them.
   It picks their mip as the pixel term + the grazing term (graz >= 0.15)
   + the face-corner term (added by HeightRange); a coarser block's
   minimum bounds every finer sample. */
static int footprintAllLand(const double dir[3], double capAngle, double maxDist)
{
	double lodK = LOD_K_NUMER / (g_bodies[SolarSystem_ActiveBody()].radius * getWindowHeight());
	double fragLod = log2(fmax(maxDist * lodK, 1e-30)) + log2(1.0 / 0.15);
	float hmin, hmax;
	TerrainGen_HeightRange(dir, capAngle, fmin(fragLod, TerrainGen_MapMaxLod()), &hmin, &hmax);
	return hmin > g_planetGen.seaLevel;
}

/* QuadTree_ReliefFn for Earth's water shell (water.vert): sea level, plus
   waves wherever there is sea. Its own walk, so the thin shell is culled
   and refined on its own instead of inheriting the seafloor's depth range.
   All-land footprints are empty (water.frag discards every fragment). */
static void waterReliefBounds(const double dir[3], double capAngle, double cell,
                              double maxDist, double *lo, double *hi)
{
	(void)cell;
	if (footprintAllLand(dir, capAngle, maxDist)) {
		*lo = 1.0;
		*hi = 0.0;
		return;
	}
	double shell = g_waveParams[0] - 1.0;
	double wave = waveExcursion(g_bodies[BODY_EARTH].radius);
	*lo = shell - wave;
	*hi = shell + wave;
}

/* The active planet as the quadtree sees it: camera-relative model, relief
   bounds for the surface being drawn (terrain or water shell), and the
   deepest seafloor as the horizon occluder (water is see-through). */
static QuadTree_Body planetQuadTreeBody(const Body *planet, QuadTree_ReliefFn relief)
{
	float gmin, gmax;
	TerrainGen_GlobalRange(&gmin, &gmax);
	QuadTree_Body b = { Body_ModelMatrix(planet, g_cam), planet->radius, relief, reliefOf(gmin) };
	return b;
}

/* Uniform + texture-unit recipe for the quadtree body passes, reproduced
   from the old drawTree so the planet and water programs render identically
   (samplers their shaders do not declare resolve to -1 and are ignored).
   QuadTree_DrawSphere owns the walk, instance buffer, and instanced draw.
   `m` and `lightPosition` are camera-relative; the body centre is m's
   translation column (bodyCenter), which the shaders subtract wherever they
   need planet-local positions (surface up, atmosphere shell). */
void setBodyUniforms(GLuint shader, float lodRadius, mat4 m, vec4 lightPosition,
                     GLuint tex0, GLuint tex2, GLuint slopeTex, GLuint dispTex, GLuint jacobTex, GLuint skyTex) {
	glUseProgram(shader);

	mat4 p = cameraProjection();
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &p.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	mat4 v = getViewMatrix();
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &v.m[0][0]);

	/* Camera-relative space: the camera is exactly the origin. */
	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "bodyCenter"), m.m[0][3], m.m[1][3], m.m[2][3]);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniform1f(glGetUniformLocation(shader, "oceanOffset"), g_waveParams[0]);
	glUniform1f(glGetUniformLocation(shader, "waveAmp"), g_waveParams[1]);
	glUniform1f(glGetUniformLocation(shader, "waveNormalStrength"), g_waveParams[2]);
	/* Per cascade: patches per body radius at its drawn size, and that
	   size over true size (heights scale with it). Water.c. */
	float tilings[OCEAN_CASCADES], scales[OCEAN_CASCADES], shoreDepths[OCEAN_CASCADES];
	for (int c = 0; c < OCEAN_CASCADES; c++) {
		scales[c] = (float)waveCascadeScale(c);
		tilings[c] = (float)(lodRadius / (waveCascadePatchM(c) * scales[c]));
		/* Water depth (m) each cascade's waves fade out over toward the
		   coast (seastate.glsl): a few of its drawn rms heights, about
		   where waves that size break. */
		shoreDepths[c] = (float)(WAVE_SHORE_RMS * OCEAN_SWELL_RMS_M * waveCascadeRmsFrac(c)
		                         * scales[c] * g_waveParams[1]);
	}
	glUniform1fv(glGetUniformLocation(shader, "waveTilings"), OCEAN_CASCADES, tilings);
	glUniform1fv(glGetUniformLocation(shader, "waveCascadeScales"), OCEAN_CASCADES, scales);
	glUniform1fv(glGetUniformLocation(shader, "waveShoreDepths"), OCEAN_CASCADES, shoreDepths);
	glUniform1f(glGetUniformLocation(shader, "waveChoppy"), g_waveParams[4]);
	glUniform1f(glGetUniformLocation(shader, "time"), (float)(App_Time()/100.0));
	/* The wave maps (tex0, slopeTex, dispTex, jacobTex) are 2D arrays, one
	   layer per cascade; the planet passes 0 for them. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, tex0);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	bindShadowMap(shader, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, tex2);
	glUniform1i(glGetUniformLocation(shader, "noiseTexture"), 2);
	glUniform1i(glGetUniformLocation(shader, "detailNormal"), 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D_ARRAY, slopeTex);
	glUniform1i(glGetUniformLocation(shader, "waveSlope"), 3);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D_ARRAY, dispTex);
	glUniform1i(glGetUniformLocation(shader, "waveDisp"), 4);
	glActiveTexture(GL_TEXTURE7);
	glBindTexture(GL_TEXTURE_2D_ARRAY, jacobTex);
	glUniform1i(glGetUniformLocation(shader, "waveJacobian"), 7);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_CUBE_MAP, skyTex);
	glUniform1i(glGetUniformLocation(shader, "skybox"), 5);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_CUBE_MAP, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*getWindowHeight()));
	glUniform1f(glGetUniformLocation(shader, "lodCellK"), (float)lodCellKFor(lodRadius));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	/* World units are metres, so the body radius is its radius in metres
	   (water.frag sizes the sub-pixel wind waves from it). */
	glUniform1f(glGetUniformLocation(shader, "planetRadiusM"), lodRadius);
	/* Sea level + ocean depth from the last bake. planet and water go through
	   this one function so they can never disagree — a different seaLevel in
	   water.frag would drift the discard edge off the drawn coastline. */
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), g_planetGen.seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), g_planetGen.oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), g_planetStyle.landRelief);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), g_planetStyle.oceanRelief);
	/* quad.frag rewrites gl_FragDepth for seafloor that pokes through the
	   water; it needs the main projection's convention and near plane, and
	   how far under sea level the wave troughs can reach. */
	glUniform1f(glGetUniformLocation(shader, "depthNear"), CAMERA_NEAR);
	glUniform1f(glGetUniformLocation(shader, "seaClearance"), (float)waveExcursion(lodRadius));
	/* Climate map (unit 8) + surface style for quad.frag's biome shading. */
	setPlanetSurfaceUniforms(shader, 8);
	/* Terrain detail (quad.vert/quad.frag), faded for the main walk. */
	TerrainDetail_SetUniforms(shader, lodRadius, QT_TARGET_TRIANGLE_PX);
	glActiveTexture(GL_TEXTURE0);
}

void drawInstanced(GLuint vao, GLuint vbo, GLuint shader, int vertexNumber, int drawAmount, vec3 *positions, mat4 *rotations, mat4 model, float *scaleArray, float theta, vec4 lightPosition) {
	vec3 cameraPos = {0.0f, 0.0f, 0.0f};   // camera-relative space

	/*for(int i = 0; i < numDraws; i++){
		float deg = i * degToRad;
		positions[i].x = cos(deg)*innerRad+(-(((float)rand()/(float)(RAND_MAX)) * width));
		positions[i].y = sin(deg)*innerRad+(-(((float)rand()/(float)(RAND_MAX)) * width));
		positions[i].z = -(((float)rand()/(float)(RAND_MAX)) * height);
	}*/

	vec3 *translation = malloc(drawAmount*sizeof(vec3));
	for(int i = 0; i < drawAmount; i++){

		translation[i].x = cos(scaleArray[i]+theta/85.0)*140.0+positions[i].x;
		translation[i].y = positions[i].y;
		translation[i].z = sin(scaleArray[i]+theta/85.0)*140.0+positions[i].z;
	}

	/*vec3 translation;
	translation.x = (65.0*1.5) * cos(theta/75.0);
	translation.y = 0.0;
	translation.z = (65.0*1.5) * sin(theta/75.0);*/
	vec3 center = {0.0, 0.0, 0.0};

	mat4 *modelArr = malloc(drawAmount*4*sizeof(vec4));
	//printf("drawAmount: %zu\n",drawAmount*sizeof(mat4));
	//printf("vertexNumber: %zu\n",vertexNumber*sizeof(mat4));
	for(int i = 0; i < drawAmount; i++) {
		//modelArr[i] = multiplymat4(multiplymat4(translatevec3(translation), rotations[i]), scale(scaleArray[i]/10.0));
		//modelArr[i] = translate(pos[i].x+65.0/2.0, pos[i].y, pos[i].z);
		modelArr[i] = multiplymat4(multiplymat4(multiplymat4(multiplymat4(translatevec3(center), rotateX(-10.0)), translatevec3(translation[i])), rotations[i]), scale(scaleArray[i]/70.0));
		//modelArr[i] = translatevec3(translation[i]);
		//modelArr[i] = multiplymat4(multiplymat4(translate(positions[i].x+65.0/2.0, positions[i].y, positions[i].z), rotations[i]), scale(scaleArray[i]/10.0));
	}
	//multiplymat4(multiplymat4(multiplymat4(positionMatrix, translatevec3(translation)), scale(15.0)),rotateX(90.0));

	glBindVertexArray(vao);
	initMVP(shader, model, getViewMatrix());

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(0));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(sizeof(vec4)));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(2*sizeof(vec4)));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(3*sizeof(vec4)));
	glEnableVertexAttribArray(6);

	glVertexAttribDivisor(3, 1);
	glVertexAttribDivisor(4, 1);
	glVertexAttribDivisor(5, 1);
	glVertexAttribDivisor(6, 1);

	glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*drawAmount, &modelArr, GL_STATIC_DRAW);

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), cameraPos.x, cameraPos.y, cameraPos.z);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	//glDrawArraysInstanced(GL_TRIANGLES, 0, vertexNumber, drawAmount);
	glBindVertexArray(0);
}

void drawTess(GLuint vao, GLuint shader, int vertices, GLuint texture, GLuint texture2, GLuint normal, GLuint displacement, mat4 m, vec3 position, vec4 lightPosition, mat4 lightSpaceMatrix) {
	glUseProgram(shader);
	initMVP(shader, m, getViewMatrix());
	glBindVertexArray(vao);

	// Camera-relative space: the camera is exactly the origin.
	glUniform3f(glGetUniformLocation(shader, "camPosition"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "translation"), position.x, position.y, position.z);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpaceMatrix.m[0][0]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture2);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "noiseTexture"), 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, depthMap);
	glUniform1i(glGetUniformLocation(shader, "depthMap"), 2);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, normal);
	glUniform1i(glGetUniformLocation(shader, "normalTex"), 3);

	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, displacement);
	glUniform1i(glGetUniformLocation(shader, "displacementTex"), 4);

	glPatchParameteri(GL_PATCH_VERTICES, vertices);
	glDrawArrays(GL_PATCHES, 0, vertices);
	glBindVertexArray(0);
}

void draw(GLuint vao, GLuint shader, int vertices, GLuint texture, GLuint normal, mat4 m, vec3 position, vec4 lightPosition, mat4 lightSpaceMatrix) {
	glDisable(GL_CULL_FACE);
	//glEnable(GL_CULL_FACE);
	//glEnable(GL_BLEND);
	//glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(shader);
	initMVP(shader, m, getViewMatrix());
	glBindVertexArray(vao);
	(void)position;

	// m, lightPosition and lightSpaceMatrix are camera-relative: the camera
	// is exactly the origin.
	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniform1f(glGetUniformLocation(shader, "time"), (float)(App_Time()/100.0));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	(void)lightSpaceMatrix;   /* g_lightSpaceMatrix, via bindShadowMap */
	bindShadowMap(shader, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, normal);
	glUniform1i(glGetUniformLocation(shader, "noiseTexture"), 2);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
	//glCullFace(GL_BACK);
	//glDisable(GL_BLEND);
}

void drawWater(GLuint vao, GLuint shader, int vertices, waves *ocean, mat4 m, vec3 position, vec4 lightPosition, mat4 lightSpaceMatrix) {
	(void)vao; (void)shader; (void)vertices; (void)ocean; (void)m; (void)position; (void)lightPosition; (void)lightSpaceMatrix;
	// Removed: the water now renders through traverseQuad() with waterShader
	// so it inherits the planet's quadtree LOD (geometric waves near the
	// camera). This stub is kept only to fail fast if it is ever called.
	printf("drawWater: deprecated, use traverseQuad with waterShader\n");
}

// TEST: flat grid to verify the FFT wave textures independently of the planet shell
static int testGridVerts = 0;

GLuint initTestGrid(float size, int n) {
	int count = n*n*6;
	vec3 *points = malloc(count*sizeof(vec3));
	vec3 *normals = malloc(count*sizeof(vec3));
	vec2 *texCoords = malloc(count*sizeof(vec2));
	int idx = 0;
	for(int i = 0; i < n; i++) {
		for(int j = 0; j < n; j++) {
			float u0 = (float)i/n, u1 = (float)(i+1)/n;
			float v0 = (float)j/n, v1 = (float)(j+1)/n;
			float x0 = (u0 - 0.5f)*size, x1 = (u1 - 0.5f)*size;
			float y0 = (v0 - 0.5f)*size, y1 = (v1 - 0.5f)*size;
			vec3 c00 = {x0, y0, 0}, c10 = {x1, y0, 0}, c01 = {x0, y1, 0}, c11 = {x1, y1, 0};
			vec2 t00 = {u0, v0}, t10 = {u1, v0}, t01 = {u0, v1}, t11 = {u1, v1};
			vec3 nrm = {0, 0, 1};
			points[idx] = c00; texCoords[idx] = t00; normals[idx] = nrm; idx++;
			points[idx] = c10; texCoords[idx] = t10; normals[idx] = nrm; idx++;
			points[idx] = c01; texCoords[idx] = t01; normals[idx] = nrm; idx++;
			points[idx] = c10; texCoords[idx] = t10; normals[idx] = nrm; idx++;
			points[idx] = c11; texCoords[idx] = t11; normals[idx] = nrm; idx++;
			points[idx] = c01; texCoords[idx] = t01; normals[idx] = nrm; idx++;
		}
	}
	testGridVerts = count;
	GLuint vao = initBuffers(points, count*sizeof(vec3), normals, count*sizeof(vec3), texCoords, count*sizeof(vec2));
	free(points); free(normals); free(texCoords);
	return vao;
}

void drawWTest(GLuint vao, GLuint shader, waves *ocean, mat4 m, float amp) {
	glUseProgram(shader);
	initMVP(shader, m, getViewMatrix());
	glBindVertexArray(vao);
	glUniform1f(glGetUniformLocation(shader, "testAmp"), amp);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, ocean->dyTexture);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	glDrawArrays(GL_TRIANGLES, 0, testGridVerts);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

/* Draws the sun marker sphere at the sun body (whose world position is the
   light's; callers downcast it camera-relative for shading). */
void rotateLight(GLuint vao, GLuint shader, int vertices, GLuint texture, const Body *sun)
{
	glUseProgram(shader);
	glBindVertexArray (vao);

	mat4 model = dmat4tomat4(Body_ModelMatrix(sun, g_cam));
	initMVP(shader, model, getViewMatrix());

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	/* light.frag always samples the shadow map; the marker sits in front of
	   the light frustum's near plane, so it always reads as lit. */
	bindShadowMap(shader, 1);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
}

/* Moon drawn as an LOD quadtree sphere (same face-grid geometry as the
   planet): the light.vert instance path computes the equirectangular UVs
   that initQuadCube used to bake, so moon.jpg frames it unchanged. Only the
   uniforms drawOrbit always set are touched; `instanced` is reset afterwards
   because ringShader is shared with the sun marker. */
void drawOrbit(GLuint shader, GLuint texture, const Body *moon, vec4 lightPosition)
{
	glUseProgram(shader);

	/* Smooth sphere: no relief, the surface itself is the horizon occluder. */
	QuadTree_Body body = { Body_ModelMatrix(moon, g_cam), moon->radius, NULL, 0.0 };
	mat4 model = dmat4tomat4(body.model);
	initMVP(shader, model, getViewMatrix());
	glUniform1i(glGetUniformLocation(shader, "instanced"), 1);

	// Camera-relative space: the camera is exactly the origin.
	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	/* Receives the planet's shadow when it passes behind it. */
	bindShadowMap(shader, 1);
	QuadTree_DrawSphere(&body, getViewProjection());
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
}

/* Shadow casters, rendered into the bound shadow target with lightSpace
   (the global map's g_lightSpaceMatrix or a cascade's).
   drawDepth: a plain (undisplaced) mesh, e.g. the moon sphere.
   drawPlanetDepth: the planet through the quadtree LOD walk with the main
   pass's height displacement and skirts (depth.vert), at trianglePx and no
   finer than minSpacing; cullNear as QuadTree_DrawShadowCasters. */
void drawDepth(GLuint vao, GLuint shader, int vertices, mat4 m, mat4 lightSpace) {
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
	glUniform1f(glGetUniformLocation(shader, "displace"), 0.0f);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpace.m[0][0]);
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
}

void drawPlanetDepth(GLuint shader, const QuadTree_Body *body, mat4 lightSpace, double minSpacing,
                     double trianglePx, int cullNear) {
	mat4 m = dmat4tomat4(body->model);
	float lodRadius = (float)body->radius;
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 1);
	glUniform1f(glGetUniformLocation(shader, "displace"), 1.0f);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpace.m[0][0]);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_CUBE_MAP, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glActiveTexture(GL_TEXTURE0 + TERRAIN_SURFACE_UNIT);
	glBindTexture(GL_TEXTURE_CUBE_MAP, g_planetGen.surfaceTex);
	glUniform1i(glGetUniformLocation(shader, "surfaceMap"), TERRAIN_SURFACE_UNIT);
	glActiveTexture(GL_TEXTURE0);
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), g_planetGen.seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), g_planetGen.oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), g_planetStyle.landRelief);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), g_planetStyle.oceanRelief);
	/* Same vertex lod inputs as setBodyUniforms, so the displaced caster
	   matches the drawn surface vertex for vertex. */
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*getWindowHeight()));
	/* The main walk's mip by distance (not the caster walk's), so casters
	   sample the very heights the receivers do. */
	glUniform1f(glGetUniformLocation(shader, "lodCellK"), (float)lodCellKFor(lodRadius));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	/* Terrain detail faded for this walk's triangles. */
	TerrainDetail_SetUniforms(shader, lodRadius, trianglePx);
	QuadTree_DrawShadowCasters(body, lightSpace, minSpacing, trianglePx, cullNear);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
}

float getDeltaTime(float lastFrame) {
	GLfloat currentFrame = glfwGetTime();
	float deltaTime = currentFrame - lastFrame;
	lastFrame = currentFrame;
	doMovement(deltaTime);
	return deltaTime;
}

vec3 *getRandomPositions(vec3 *positions, int numDraws) {
	float width = 30.0;
	float height = 1.0;

	for(int i = 0; i < numDraws; i++){
		positions[i].x = -(((float)rand()/(float)(RAND_MAX)) * width);
		positions[i].y = -((float)rand()/(float)(RAND_MAX)) * height;
		positions[i].z = -(((float)rand()/(float)(RAND_MAX)) * width);
	}

	return positions;
}

mat4 *getRandomRotations(mat4 *rotations, int numDraws) {
	vec3 xAxis = {1.0, 0.0, 0.0};
	vec3 yAxis = {0.0, 1.0, 0.0};
	vec3 zAxis = {0.0, 0.0, 1.0};

	quaternion *xRot = malloc(numDraws*sizeof(quaternion));
	quaternion *yRot = malloc(numDraws*sizeof(quaternion));
	for(int i = 0; i < numDraws; i++){
		xRot[i] = angleAxis((-(((float)rand()/(float)(RAND_MAX)) * M_PI)), xAxis, zAxis);
		yRot[i] = angleAxis((-(((float)rand()/(float)(RAND_MAX)) * M_PI)), yAxis, zAxis);
	}

	for(int i = 0; i < numDraws; i++) {
		rotations[i] = quaternionToRotation(quatMultiply(xRot[i], yRot[i]));
	}

	return rotations;
}

time_t getFileLastChangeTime(const char *path) {
	struct stat file_stat;
	int err = stat(path, &file_stat);
	if(err != 0) {
		perror(" [file_is_modified] stat");
        exit(0);
	}
	return file_stat.st_mtime;
}

int checkFileChange(const char *path, time_t oldMTime) {
	struct stat file_stat;
	int err = stat(path, &file_stat);
	if(err != 0) {
		perror(" [file_is_modified] stat");
        exit(0);
	}
	return file_stat.st_mtime > oldMTime;
}

int checkShaderChange(GLuint shader, char *vert, char *frag, time_t vertTime, time_t fragTime) {
	if(checkFileChange(vert, vertTime)) {
		printf("FILE CHANGED\n");
		return 1;
	}
	if(checkFileChange(frag, fragTime)) {
		printf("FILE CHANGED\n");
		return 1;
	}
	return 0;
}

/*void *testThread(void *dx) {
	complex *t_dx = (complex*)dx;
	for(int i = 0; i < 100; i++) {
		printf("%f, %f\n", t_dx->real, t_dx->im);
	}
	free(dx);
	return NULL;
}*/

int main(int argc, char *argv[])
{
	struct obj rockObj[3];
	/* Simulation clock (seconds, double): drives every orbit and spin. */
	double simTime = 0.0;
	//printf("\tWorkdir: %s\n", getenv("PWD"));
	{
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		char *slash = strrchr(exePath, '\\');
		if(slash) *slash = '\0';
		chdir(exePath);
	}
	Bench_ParseArgs(argc, argv);
	Nebula_ParseArgs(argc, argv);
	GLFWwindow *window = setupGLFW();
	if (window == NULL)
    {
        printf("Failed to create GLFW window.");
        glfwTerminate();
        return -1;
    }
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        printf("Failed to initialize GLAD");
        return -1;
    }
	/* The main pass's reversed-Z depth (setReversedDepth) needs clip control. */
	if (!GLAD_GL_ARB_clip_control || !glClipControl)
	{
		fprintf(stderr, "GL_ARB_clip_control is required (reversed-Z depth) but not supported by this driver.\n");
		glfwTerminate();
		return -1;
	}
	if (Bench_Active())
		glfwSwapInterval(0);   /* no vsync: frames run as fast as they can */
	/* cube.exe --bake-sky <name>: bake the nebula sky's faces and exit. */
	if (Nebula_BakeRequested()) {
		int rc = Nebula_RunBake();
		glfwTerminate();
		return rc;
	}

	// Show the loading screen before the first blocking load, so the window
	// never sits blank/unresponsive while shaders compile, textures and models
	// decode, and the heightmap bakes. glfwPollEvents inside each frame keeps
	// the window responsive (Esc works) during loading.
	LoadingScreen_Init(window);
	LoadingScreen_Render(0.0f);

	GLuint sunNoiseTexture;

	struct waves ocean;
	initializeWaves(&ocean, 256);
	LoadingScreen_Step();

	GLuint tessShader = initTessShader();
	GLuint atmosphereShader = initAtmosphereShader();
	GLuint ringShader = initLightingShader();
	GLuint waterShader = initWaterShader();
	GLuint wTestShader = initWTestShader();
	GLuint testQuadVAO = initTestGrid(40.0, 96);
	GLuint depthShader = initDepthShader();
	GLuint fboShader = initFramebufferShader();
	GLuint instanceShader = initInstanceShader();
	GLuint quadShader = initQuadShader(0);
	GLuint quadDepthShader = initQuadShader(1);
	GLuint planetShader = initPlanetShader();
	attachGUIShaders();
	uiInit();
	uiSetupCameraSliders();
	DebugGUI_Init(window);
	LoadingScreen_Step(); // all shader programs
	/* The quadtree body LOD: six face grids + shared instance VBO, attribute
	   layout wired once (all three body programs declare vPosition at 0 and
	   the instance matrix at 4-7). */
	QuadTree_InitGL();
	Prof_Init();
	if (Bench_Active())
		Prof_EnableRawRecording();
	LoadingScreen_Step();

	initSun();
	Planets_Init();
	LoadingScreen_Step();

	initSkybox();
	LoadingScreen_Step();
	Nebula_Init();
	LoadingScreen_Step();
	/* A baked nebula sky (g_nebulaSkyName) also loads its settings. */
	GLuint skyboxTexture = loadSkyboxCube();
	LoadingScreen_Step();

	GLuint moonTex = loadTexture("../assets/moon.jpg", 0);
	LoadingScreen_Step();
	GLuint sphereVAO = initSphere();
	initAtmosphere(atmosphereShader, planet);
	GLuint ringVAO = initRing();
	LoadingScreen_Step();
	/* Ring rocks (Rings.c). Each mesh keeps its own obj: the vertex counts
	   differ. */
	GLuint rockVAOs[3];
	int rockVerts[3];
	rockVAOs[0] = initObjectBuffer("../assets/rock.obj", &rockObj[0]);
	LoadingScreen_Step();
	rockVAOs[1] = initObjectBuffer("../assets/rock2.obj", &rockObj[1]);
	LoadingScreen_Step();
	rockVAOs[2] = initObjectBuffer("../assets/rock3.obj", &rockObj[2]);
	LoadingScreen_Step();
	for (int i = 0; i < 3; i++)
		rockVerts[i] = rockObj[i].vertexNumber;

	GLuint quadCubeVAO = initQuadCube(30);
	LoadingScreen_Step();

	GLuint depthbuffer = initDepthbuffer();
	Shadows_InitGL();
	GLuint sunFramebuffer = initFramebuffer(&sunNoiseTexture, 512, 512);
	LoadingScreen_Step();
	//GLuint quadVAO = initQuadVAO();

	/* Bake the planet height field: GPU shaping + CPU erosion + sea level
	   stats (TerrainGen.c), from g_terrainBake. The seed advances on every
	   H-key re-bake; the debug panel edits the rest and re-bakes on demand. */
	planetHeightTex = generatePlanetHeightmap(&g_terrainBake, &g_planetGen, LoadingScreen_Step);
	TerrainDetail_Init();

	/* Procedural rings, seeded with the planet (re-rolled with it when the
	   seed changes). Every program that samples the ring shadow is a
	   receiver. */
	Rings_Init(g_terrainBake.seed, rockVAOs, rockVerts);
	Rings_AttachReceiver(quadShader);
	Rings_AttachReceiver(waterShader);
	Rings_AttachReceiver(ringShader);
	Rings_AttachReceiver(atmosphereShader);
	Clouds_Init();

	/* The procedural system (SolarSystem.c): Earth as baked above, every
	   other planet from the system seed (baked on approach). */
	SolarSystem_Init();

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	int instancedDraws = 1;

	vec3 *pos1 = malloc(instancedDraws*sizeof(vec3));
	*pos1 = *getRandomPositions(pos1, instancedDraws);
	vec3 *pos2 = malloc(instancedDraws*sizeof(vec3));
	*pos2 = *getRandomPositions(pos2, instancedDraws);
	vec3 *pos3 = malloc(instancedDraws*sizeof(vec3));
	*pos3 = *getRandomPositions(pos3, instancedDraws);

	mat4 *rotations = malloc(instancedDraws*sizeof(mat4));
	*rotations = *getRandomRotations(rotations, instancedDraws);

	float *scaleArray = malloc(instancedDraws*sizeof(float));
	for(int i = 0; i < instancedDraws; i++){
		scaleArray[i] = -((float)rand()/(float)(RAND_MAX)) * 360.0;
	}

	mat4 model;
	glViewport(0, 0, getWindowWidth(), getWindowHeight());

	float deltaTime = 0.0;
	float lastFrame = 0.0;
	g_waveParams[0] = 1.0f;   // shell radius = sea level (planet coast is at 1.0)
	g_waveParams[1] = 0.25f;  // ~0.0006 rms: well under the land relief
	g_waveParams[2] = 1.0f;
	g_waveParams[3] = 10.0f;  // wave scale: patches 10x true size (stylized)
	g_waveParams[4] = 1.0f;

	/*char *vertCheck = "shaders/water.vert";
	char *fragCheck = "shaders/water.frag";
	time_t vertTime = getFileLastChangeTime(vertCheck);
	time_t fragTime = getFileLastChangeTime(fragCheck);*/

	GLuint dxWaveTex;
	glGenTextures(1, &dxWaveTex);
	GLuint dyWaveTex;
	glGenTextures(1, &dyWaveTex);
	GLuint dzWaveTex;
	glGenTextures(1, &dzWaveTex);
	float fpsFrames= 0.0;
	float lastTime = 0.0;

	// Force one final full-bar frame before handing over to the scene.
	LoadingScreen_Finish();
	glViewport(0, 0, getWindowWidth(), getWindowHeight());

	while(!glfwWindowShouldClose(window))
	{
		/*if(checkShaderChange(tessShader, vertCheck, fragCheck, vertTime, fragTime)) {
			waterShader = initWaterShader();
			vertTime = getFileLastChangeTime(vertCheck);
			fragTime = getFileLastChangeTime(fragCheck);
		}*/
		fpsFrames++;

		/* H: re-roll the active planet's terrain seed and re-bake it; the
		   debug panel's regenerate button re-bakes with its current
		   settings (1-3 s hitch either way). A new system seed re-generates
		   every planet but Earth. */
		int rebake = DebugGUI_ConsumeRegenerate();
		if (consumeRegenRequest()) {
			g_terrainBake.seed++;
			rebake = 1;
		}
		if (rebake)
			SolarSystem_RebakeActive(NULL);
		if (DebugGUI_ConsumeNewSystem()) {
			SolarSystem_Generate(g_systemSeed);
			/* The focused planet may have a new size: back off to see it. */
			resetCameraForFocus(CAMERA_FOCUS_DISTANCE_RADII * g_bodies[g_focusBody].radius);
		}

		/* Ocean waves: only Earth has water. */
		if (SolarSystem_ActiveBody() == BODY_EARTH) {
			Prof_Begin(PROF_WAVES);
			generateWaves(&ocean);
			Prof_End(PROF_WAVES);
		}

		GLfloat currentFrame = (GLfloat)App_Time();
		deltaTime = currentFrame - lastFrame;
		lastFrame = currentFrame;
		doMovement(deltaTime);

		/* Advance the bodies (R pauses). The step is capped so a hitch (the
		   first frame after loading, a re-bake) does not jump hours ahead.
		   Bench runs pin simTime to the frame index: repeatable lighting. */
		if (Bench_Active())
			simTime = Bench_SimTime();
		else if (!getRotationPaused())
			simTime += fmin(deltaTime, 0.1) * g_simTimeScale;
		Bodies_Update(simTime);

		/* Focus body (Tab / debug panel): the camera rides with it. A
		   switch re-centres the orbit on the new body. */
		{
			static int lastFocus = BODY_EARTH;
			int step = consumeFocusStep();
			if (step != 0 && !Bench_Active())
				g_focusBody = ((g_focusBody + step) % BODY_COUNT + BODY_COUNT) % BODY_COUNT;
			const Body *f = &g_bodies[g_focusBody];
			dvec3 toSun = dsubvec3(g_bodies[BODY_SUN].position, f->position);
			double ds = dlengthvec3(toSun);
			dvec3 sunDir = ds > 0.0 ? dscalarvec3(toSun, 1.0 / ds) : (dvec3){ 1.0, 0.0, 0.0 };
			setCameraFocus(f->position, f->radius, sunDir);
			if (g_focusBody != lastFocus) {
				resetCameraForFocus(CAMERA_FOCUS_DISTANCE_RADII * f->radius);
				lastFocus = g_focusBody;
			}
		}
		Bench_UpdateCamera();

		/* This frame's floating origin (see camRelTranslate). */
		g_cam = getCameraPosition();

		/* The active planet (SolarSystem.c): the terrain planet drawn in full,
		   the largest on screen. One never seen up close bakes in the
		   background first (a few seconds as a stand-in; cached after).
		   Bench runs switch and bake at once: repeatable frames. */
		SolarSystem_Update(g_cam, g_focusBody, Bench_BlockingBakes());
		/* Cloud weather maps for the planets that have them (a build or
		   two per frame, as simulation time moves on). */
		Clouds_Update(g_simTime);
		int activeId = SolarSystem_ActiveBody();
		int isEarth = activeId == BODY_EARTH;
		const Body *planetB = &g_bodies[activeId];
		const Body *sun = &g_bodies[BODY_SUN];
		const Body *moon = &g_bodies[BODY_MOON];

		/* Full passes for the active planet and the Moon only while they are
		   more than a few pixels across; Planets_Draw stands in otherwise.
		   The Moon needs Earth's shadow map, so only with Earth active. */
		int planetFull = Planets_FullDetail(planetB, g_cam);
		int moonFull = isEarth && Planets_FullDetail(moon, g_cam);

		if(currentFrame - lastTime >= 1.0)
		{
			//printf("%f\n", fpsFrames);
			fpsFrames = 0.0;
			lastTime += 1.0;
		}

		/* Planet radius (metres) and world centre. */
		float fScale = (float)planetB->radius;
		dvec3 translation = planetB->position;

		/* Atmosphere: shell starts at the planet surface; the rest of its
		   parameters live in g_atmosphere (Atmosphere.c, debug panel).
		   Shared by the dome (in-scatter) and the planet/water passes
		   (extinction). */
		float atmoInner = fScale;

		//int terrainMaxLOD = (int)(log(fScale)/log(2));

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.0, 0.0, 0.0, 1.0);

		//Light calculations per frame: the sun body is the light
		rotateLight(quadCubeVAO, ringShader, qc.vertexNumber, sunNoiseTexture, sun);
		dvec3 lightWorld = sun->position;
		/* Shading and shadows both use the camera-relative light. */
		vec3 lightPositionXYZ = toCamRel(lightWorld);
		vec4 lightPosition = {lightPositionXYZ.x, lightPositionXYZ.y, lightPositionXYZ.z, 1.0};

		/* Ring frame for this camera + light; ring-shadow receivers that are
		   not set up through setBodyUniforms bind here. */
		Rings_SetFrame(translation, g_cam, fScale, lightPosition);
		Rings_BindReceiver(ringShader);
		Rings_BindReceiver(atmosphereShader);

		/* Planet model (scale * spin/tilt, camera-relative), composed in
		   double and downcast once; shared by the shadow casters and the
		   main pass. */
		QuadTree_Body planetBody = planetQuadTreeBody(planetB, terrainReliefBounds);
		/* Land carries detail noise (quad.vert, depth.vert): widen its
		   bounds and skirts. The water shell does not. */
		planetBody.detailAmp = TerrainDetail_Amplitude(planetB->radius);
		planetBody.detailSeam = TerrainDetail_Seam(planetB->radius);
		mat4 planetModel = dmat4tomat4(planetBody.model);
		/* New frame: the walk cache must not outlive this frame's inputs. */
		QuadTree_BeginFrame();

		/* Shadow frustum: the sun is far enough away to be a directional
		   light, so an orthographic box along the sun direction, just wide
		   enough for the planet's bounding sphere (land relief + margin) so
		   all SHADOW_MAP_SIZE^2 texels land on the planet. Its eye sits a
		   finite distance up-sun (never at the sun itself: a 1 AU float
		   translation would quantise depth to kilometres). Depth spans the
		   moon's orbit on either side so it can both eclipse the planet and
		   be eclipsed by it. */
		{
			dvec3 toSun = dsubvec3(lightWorld, translation);
			dvec3 sunDir = dscalarvec3(toSun, 1.0 / dlengthvec3(toSun));
			double boundR = fScale * (1.0 + g_planetStyle.landRelief) * SHADOW_BOUND_MARGIN;
			/* Earth's Moon both casts and receives; other planets only self-shadow. */
			double reach = (isEarth ? dlengthvec3(dsubvec3(moon->position, translation)) + moon->radius * 1.5 : 0.0) + boundR;
			dvec3 eye = daddvec3(translation, dscalarvec3(sunDir, reach));
			mat4 lightProjection = ortho((float)-boundR, (float)boundR, (float)-boundR, (float)boundR, 0.0f, (float)(2.0 * reach));
			mat4 lightView = getLightViewMatrix(eye, translation);
			g_lightSpaceMatrix = multiplymat4(lightProjection, lightView);
			g_shadowTexel = (float)(2.0 * boundR / SHADOW_MAP_SIZE);
		}
		mat4 lightSpaceMatrix = g_lightSpaceMatrix;

		/* Shadow pass. The ring is not a caster: its shadow is semi-
		   transparent, so the receivers compute it analytically from the
		   ring profile instead (ringTransmittance). A slope-scaled polygon
		   offset handles the remaining grazing acne together with the
		   receiver offsets in sunVisibility. */
		/* Only while the planet or the Moon is drawn in full (the Moon receives Earth's shadow). */
		if (planetFull || moonFull) {
			Prof_Begin(PROF_SHADOW);
			glBindFramebuffer(GL_FRAMEBUFFER, depthbuffer);
				glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
				glClear(GL_DEPTH_BUFFER_BIT);
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(1.5f, 2.0f);
				model = dmat4tomat4(Body_ModelMatrix(moon, g_cam));
				if (isEarth)
					drawDepth(quadCubeVAO, depthShader, qc.vertexNumber, model, g_lightSpaceMatrix);
				/* Coarse walk, no finer than half a texel: smaller detail cannot
				   show in this map. */
				drawPlanetDepth(depthShader, &planetBody, g_lightSpaceMatrix, 0.5 * g_shadowTexel,
				                QT_SHADOW_TRIANGLE_PX, 1);

				/* Cascades: finer maps around the camera (Shadows.c), fitted
				   between the nearest and farthest terrain in view. Casters use
				   the main pass's LOD, so they are the very mesh that receives
				   (minus what each box culls), and keep everything up-sun of the
				   box (clamped onto its near plane). */
				{
					double nearest, farthest;
					terrainViewRange(planetB, &nearest, &farthest);
					mat4 view = getViewMatrix();
					dvec3 forward = { -view.m[2][0], -view.m[2][1], -view.m[2][2] };
					dvec3 toSun = dsubvec3(lightWorld, translation);
					Shadows_Fit(g_cam, forward, dscalarvec3(toSun, 1.0 / dlengthvec3(toSun)), nearest, farthest);
				}
				for (int i = 0; i < g_cascades.count; i++) {
					const ShadowCascade *sc = &g_cascades.c[i];
					Shadows_BeginCascade(i);
					if (isEarth)
						drawDepth(quadCubeVAO, depthShader, qc.vertexNumber, model, sc->lightSpace);
					drawPlanetDepth(depthShader, &planetBody, sc->lightSpace, 0.5 * sc->texel,
					                QT_TARGET_TRIANGLE_PX, 0);
				}
				Shadows_EndCascades();
				glDisable(GL_POLYGON_OFFSET_FILL);
			Prof_End(PROF_SHADOW);
		}

		/* Main pass: the multisampled float-depth scene target, reversed-Z
		   (see setReversedDepth), resolved into the window before the UI. */
		/* Nebula presence grid on a settings change; Rebake / Save
		   requests from the debug panel (blocking). */
		Nebula_Update();

		int sceneW = (int)getWindowWidth(), sceneH = (int)getWindowHeight();
		ensureSceneTarget(sceneW, sceneH);
		glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFBO);
		glViewport(0, 0, sceneW, sceneH);
		setReversedDepth(1);

		glClearColor(0.1, 0.0, 0.2, 1.0);
		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

		//Skybox: full-screen background, drawn first (far plane, no depth write)
		mat4 skyboxProj = cameraProjection();
		drawSkybox(Nebula_SkyTexture(skyboxTexture), skyboxProj, getViewRotation());

		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		if (moonFull) {
			Prof_Begin(PROF_MOON);
			drawOrbit(ringShader, moonTex, moon, lightPosition);
			Prof_End(PROF_MOON);
		}
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		//Planet
		//drawTess(quadCubeVAO, tessShader, qc.vertexNumber, textureColorBuffer, dxWaveTex, planetNorm, planetDisp, m, translation, lightPosition, lightSpaceMatrix);
		//Water: drawn after the planet, shell at sea level (see drawWater)
		//Sun (billboard): opaque disc drawn with the scene; glow drawn last (below)
		mat4 sunProj = cameraProjection();
		mat4 sunView = getViewMatrix();
		float sunGlowRadius = (float)(sun->radius * SUN_GLOW_RADII);
		drawSunDisc(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

		//Distant bodies: every planet not drawn in full this frame (gas
		//giants always; terrain planets and the Moon while small or
		//inactive).
		Planets_Draw(sunProj, sunView, g_cam, planetFull ? activeId : -1, moonFull);

		//Quadtree planet: LOD sphere built from six instanced face grids,
		//centred camera-relative (the walk runs with the camera at the origin).
		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		model = planetModel;
		mat4 viewProj = getViewProjection();

		//Planet (land + seafloor): drawn first so the translucent water can
		//blend over the seafloor.
		//Depth prepass, then colour: the prepass lays down the surface depth
		//(seafloor pushed behind the water) with a cheap program; the full
		//shader then runs only where its fragment is the visible one (depth
		//test >=, no depth writes, so early-Z holds). The second draw reuses
		//the walk.
		if (planetFull) {
			Prof_Begin(PROF_PLANET);
			setBodyUniforms(quadDepthShader, fScale, model, lightPosition,
			                0, dxWaveTex, 0, 0, 0, 0);
			/* Per MSAA sample: a shader that writes gl_FragDepth writes one
			   value for the whole pixel, and the colour pass's per-sample >=
			   test would then fail on about half the samples of every sloped
			   surface (resolving to see-through terrain). The prepass is cheap,
			   so running it per sample costs little. */
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glEnable(GL_SAMPLE_SHADING);
			glMinSampleShading(1.0f);
			QuadTree_DrawSphere(&planetBody, viewProj);
			glDisable(GL_SAMPLE_SHADING);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

			setBodyUniforms(quadShader, fScale, model, lightPosition,
			                0, dxWaveTex, 0, 0, 0, 0);
			setOceanUniforms(quadShader, 10);   /* beach swash exposure */
			setAtmosphereExtinction(quadShader, atmoInner, &g_atmosphere);
			Rings_BindReceiver(quadShader);
			Clouds_BindReceiver(quadShader, activeId);
			glDepthFunc(GL_GEQUAL);   /* reversed-Z: nearer = greater */
			glDepthMask(GL_FALSE);
			QuadTree_DrawSphere(&planetBody, viewProj);
			glDepthMask(GL_TRUE);
			glDepthFunc(GL_GREATER);
			Prof_End(PROF_PLANET);
		}

		//Water (Earth only for now): its own quadtree walk over the thin shell, FFT-
		//displaced sea-level shell that discards over land in-shader
		//(FFT cascades: waveHeight/Disp/Slope arrays + skybox reflection in
		//water.frag). Alpha-blended: water.frag fades to clear in the
		//shallows from the per-pixel water depth.
		if (planetFull && isEarth) {
			Prof_Begin(PROF_WATER);
			setBodyUniforms(waterShader, fScale, model, lightPosition,
			                ocean.dyTexture, 0,
			                ocean.slopeTexture, ocean.dispTexture, ocean.jacobTexture, skyboxTexture);
			/* Also feeds the sky water.frag reflects (E, G_M: same as the dome). */
			setAtmosphereExtinction(waterShader, atmoInner, &g_atmosphere);
			glUniform1f(glGetUniformLocation(waterShader, "waveGeomScale"), (float)waveGeomScale(planetB->radius));
			/* Wind / fetch sea state. Unit 10: 0-9 are taken (Rings uses 9). */
			setOceanUniforms(waterShader, 10);
			Rings_BindReceiver(waterShader);
			Clouds_BindReceiver(waterShader, activeId);
			/* Water.frag's back-face early-out is only safe from outside the
			   shell; the surface view (camera inside) must still see it. */
			double shellCamDist = dlengthvec3(dsubvec3(g_cam, translation));
			glUniform1f(glGetUniformLocation(waterShader, "outsideShell"),
			            shellCamDist > fScale*1.05 ? 1.0f : 0.0f);
			/* Its own walk and bounds: the thin shell (land footprints culled). */
			QuadTree_Body waterBody = planetQuadTreeBody(planetB, waterReliefBounds);
			waterBody.trianglePx = QT_WATER_TRIANGLE_PX;
			glEnable(GL_BLEND);
			/* Premultiplied: water.frag adds its surface reflection over
			   the seafloor where the column is clear. */
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			QuadTree_DrawSphere(&waterBody, viewProj);
			glDisable(GL_BLEND);
			Prof_End(PROF_WATER);
		}
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		//Clouds: the layer as a slab over the planet (Clouds.c), blended over
		//the land and water, before the rings (which must draw over it).
		//From here on the scene's alpha holds the cloud cover the dome and
		//the sun's glow scale their light by: cleared to 0 here (every
		//frame: the glow always reads it), written only by the clouds (the
		//rings mask it off).
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (planetFull) {
			Prof_Begin(PROF_CLOUDS);
			Clouds_Draw(sunProj, sunView, g_cam, activeId);
			Prof_End(PROF_CLOUDS);
		}

		//TEST: flat animated wave grid above the planet (debug for FFT water)
		/*{
			vec3 testPos = {0.0, 0.0, 60.0};
			mat4 testModel = translatevec3(testPos);
			drawWTest(testQuadVAO, wTestShader, &ocean, testModel, 300.0);
		}*/

		//Rings: opaque close-up rocks, then the analytic sheet and the dust
		//(premultiplied, depth-tested, no depth write). Before the
		//atmosphere dome, which takes the sheet's coverage out of its glow.
		if (planetFull) {
			Prof_Begin(PROF_RINGS);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   /* the cloud cover (above) */
			Rings_DrawRocks(sunProj, sunView);
			Rings_DrawSheet(sunProj, sunView);
			Rings_DrawDust(sunProj, sunView);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			Prof_End(PROF_RINGS);
		}

		//Nebula: the in-system wisps (Nebula.c), marched to the scene
		//depth and blended over everything drawn so far; the dome adds
		//its air in front afterwards. Not tied to planetFull: deep space
		//is where it matters most. The alpha channel keeps the cloud
		//cover (above), as for the rings.
		if (g_nebulaEnabled) {
			Prof_Begin(PROF_NEBULA);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			Nebula_Draw(sunProj, sunView, g_cam);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			Prof_End(PROF_NEBULA);
		}

		//Atmosphere dome: OpenGLSolar port (GPU Gems ch.16 scattering on the
		//planet shell; params above with the planet extinction). Rays end at
		//the scene depth, so this runs after everything opaque.
		if (planetFull && g_atmosphere.shellHeight > 0.0f) {
			Prof_Begin(PROF_ATMOSPHERE);
			drawAtmosphereDome(atmosphereShader, sunProj, sunView, camRelTranslate(translation),
			                   atmoInner, &g_atmosphere, lightPosition);
			Prof_End(PROF_ATMOSPHERE);
		}

		//Sun corona halo — must be drawn LAST (additive, does not write depth)
		drawSunGlow(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

		/* Resolve the scene into the window; the UI draws there directly,
		   with the standard depth convention restored. */
		Prof_Begin(PROF_OVERLAY);
		setReversedDepth(0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sceneFBO);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, sceneW, sceneH, 0, 0, sceneW, sceneH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		//Camera speed sliders: UI overlay, drawn over everything. Laid out
		//every frame so they track window resizes.
		{
			int ww, wh;
			glfwGetWindowSize(window, &ww, &wh);
			uiLayoutSliders(ww, wh, UI_SLIDER_COUNT);
		}
		Bench_MaybeCapture();
		uiDrawAll();

		//Debug panel (F1): drawn last, over the sliders.
		DebugGUI_Frame();
		Prof_End(PROF_OVERLAY);
		Prof_EndFrame();
		if (Bench_NoteFrame())
			glfwSetWindowShouldClose(window, GL_TRUE);

		glfwPollEvents();
		glfwSwapBuffers(window);
	}
	Bench_Finalize();
	DebugGUI_Shutdown();
	glfwTerminate();
	return 0;
}
