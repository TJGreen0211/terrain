#include "Main.h"
#include "Sun.h"
#include "Skybox.h"
#include "Atmosphere.h"
#include "LoadingScreen.h"
#include "GUI.h"
#include "TerrainGen.h"
#include <stdio.h>
//#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <pthread.h>

float zNear = 0.5, zFar = 100000.0;
GLuint depthMap;
/* Sun shadow map: a square depth texture rendered from the sun's position
   (it is a point light, so the light projection is a perspective frustum
   from the sun just wide enough to enclose the planet). Its resolution is
   independent of the window. */
#define SHADOW_MAP_SIZE 4096
/* Shadow-casting moon (moonWorldPosition / drawOrbit): orbit radius and
   body radius in world units. The light frustum's depth range must reach
   the moon on both sides of the planet. */
#define MOON_ORBIT  2000.0
#define MOON_RADIUS 100.0
/* Per-frame extras for the quadtree body passes (set once per frame, read by
   setBodyUniforms / bindShadowMap). lightSpace maps CAMERA-RELATIVE
   positions to the shadow map's clip space; shadowTexel is the world size of
   one shadow texel at the planet (scales the receiver bias). */
mat4 g_lightSpaceMatrix;
float g_shadowTexel = 1.0f;
float g_waveParams[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // oceanOffset, waveAmp, normalStrength, tiling, choppiness
/* Baked planet height map (2048x1024 R16F + mips), sampled by quad.frag/
   water.frag instead of evaluating simplex noise per pixel.

   The height field lives on the unit sphere: a screen pixel at surface
   distance dist covers dist*thetaPixel world units, i.e. dist*thetaPixel/r
   radians on the sphere, while one texel covers PI/1024 radians. With
   thetaPixel = 45deg/H that gives lod = log2(dist * LOD_K/(r*H)) plus the
   pole/grazing corrections in the shaders. */
GLuint planetHeightTex;
#define LOD_K_NUMER (0.7853982f*1024.0f/3.1415926f)
/* Geometric land relief as a fraction of the planet radius (quad.vert /
   depth.vert displace the unit sphere by it). Sea level is radius 1.0 for
   both the planet and the water shell. The seafloor sinks to OCEAN_RELIEF
   below it along a squared (continental shelf) profile, so vertices next to
   the coast stay near sea level and coast-straddling triangles barely sag. */
#define LAND_RELIEF 0.025f
#define OCEAN_RELIEF 0.01f
/* Main-pass clip planes (setBodyUniforms' projection); quad.frag needs them
   to rewrite gl_FragDepth for seafloor that pokes through the water. */
#define BODY_NEAR 0.5f
#define BODY_FAR  100000.0f
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

GLuint initQuadShader() {
	GLuint shader;
	createShader(&shader, "shaders/quad.vert",
		"shaders/quad.frag");
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
   stays usable: t in [0,1] -> min*(max/min)^t. Ranges keep the camera
   defaults (MovementSpeed 200, CameraScrollSpeed 10) at the same relative
   spot OpenGLSolar's do. */
#define UI_SLIDER_MOVEMENT 0
#define UI_SLIDER_SCROLL   1
#define UI_SLIDER_COUNT    2

#define CAMERA_MOVEMENT_MIN 5.0f
#define CAMERA_MOVEMENT_MAX 2000.0f
#define CAMERA_SCROLL_MIN 0.5f
#define CAMERA_SCROLL_MAX 200.0f

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
   ship): sampler unit, light-space matrix and texel size. */
static void bindShadowMap(GLuint shader, int unit) {
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, depthMap);
	glUniform1i(glGetUniformLocation(shader, "depthMap"), unit);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &g_lightSpaceMatrix.m[0][0]);
	glUniform1f(glGetUniformLocation(shader, "shadowTexel"), g_shadowTexel);
	glActiveTexture(GL_TEXTURE0);
}

/* Main camera projection * view, matching initMVP/setBodyUniforms; the
   quadtree walk frustum-culls its nodes against it. */
static mat4 getViewProjection(void) {
	return multiplymat4(perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar), getViewMatrix());
}

void initMVP(int shader, mat4 m, mat4 v) {
	mat4 p = perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &p.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &v.m[0][0]);
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

	mat4 p = perspective(45.0, getWindowWidth()/getWindowHeight(), BODY_NEAR, BODY_FAR);
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
	glUniform1f(glGetUniformLocation(shader, "waveTiling"), g_waveParams[3]);
	glUniform1f(glGetUniformLocation(shader, "waveChoppy"), g_waveParams[4]);
	glUniform1f(glGetUniformLocation(shader, "time"), glfwGetTime()/100.0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex0);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	bindShadowMap(shader, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, tex2);
	glUniform1i(glGetUniformLocation(shader, "noiseTexture"), 2);
	glUniform1i(glGetUniformLocation(shader, "detailNormal"), 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, slopeTex);
	glUniform1i(glGetUniformLocation(shader, "waveSlope"), 3);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, dispTex);
	glUniform1i(glGetUniformLocation(shader, "waveDisp"), 4);
	glActiveTexture(GL_TEXTURE7);
	glBindTexture(GL_TEXTURE_2D, jacobTex);
	glUniform1i(glGetUniformLocation(shader, "waveJacobian"), 7);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_CUBE_MAP, skyTex);
	glUniform1i(glGetUniformLocation(shader, "skybox"), 5);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*getWindowHeight()));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	/* Sea level + ocean depth from the last bake. planet and water go through
	   this one function so they can never disagree — a different seaLevel in
	   water.frag would drift the discard edge off the drawn coastline. */
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), g_planetGen.seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), g_planetGen.oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), LAND_RELIEF);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), OCEAN_RELIEF);
	glUniform2f(glGetUniformLocation(shader, "depthPlanes"), BODY_NEAR, BODY_FAR);
	/* Climate map (unit 8) + surface style for quad.frag's biome shading. */
	setPlanetSurfaceUniforms(shader, 8);
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
	glUniform1f(glGetUniformLocation(shader, "time"), glfwGetTime()/100.0);
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
	glBindTexture(GL_TEXTURE_2D, ocean->dyTexture);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	glDrawArrays(GL_TRIANGLES, 0, testGridVerts);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Draws the sun marker and returns the light's WORLD position (double):
   callers downcast it camera-relative for shading, and build the shadow
   light view from the world value. */
dvec3 rotateLight(GLuint vao, GLuint shader, int vertices, GLuint texture, float theta, dvec3 position)
{
	glUseProgram(shader);
	glBindVertexArray (vao);

	dvec3 translation;
	translation.x = (-4000.0);// * cos(theta/75.0);
	translation.y = 0.0;
	translation.z = (-4000.0);// * sin(theta/75.0);
	dvec3 world = daddvec3(position, translation);

	mat4 model = multiplymat4(multiplymat4(camRelTranslate(world), scale(15.0)),rotateX(90.0));
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

	return world;
}

/* Moon drawn as an LOD quadtree sphere (same face-grid geometry as the
   planet): the light.vert instance path computes the equirectangular UVs
   that initQuadCube used to bake, so moon.jpg frames it unchanged. Only the
   uniforms drawOrbit always set are touched; `instanced` is reset afterwards
   because ringShader is shared with the sun billboard and the ship draw. */
/* World position of the moon's centre (orbiting the planet at `position`). */
static dvec3 moonWorldPosition(float theta, dvec3 position)
{
	dvec3 translation;
	translation.x = MOON_ORBIT * cos(theta/100.0);
	translation.y = 0.0;
	translation.z = MOON_ORBIT * sin(theta/100.0);
	return daddvec3(position, translation);
}

void drawOrbit(GLuint shader, GLuint texture, float theta, dvec3 position, vec4 lightPosition)
{
	glUseProgram(shader);

	mat4 model = multiplymat4(multiplymat4(camRelTranslate(moonWorldPosition(theta, position)), scale(MOON_RADIUS)),rotateX(90.0));
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
	QuadTree_DrawSphere(model, (float)MOON_RADIUS, getViewProjection());
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
}

/* Shadow casters, rendered into the bound shadow FBO with g_lightSpaceMatrix.
   drawDepth: a plain (undisplaced) mesh, e.g. the moon sphere.
   drawPlanetDepth: the planet through the same quadtree LOD walk, height
   displacement and skirts as the quad.vert main pass, so the terrain casts
   from exactly the surface that receives (no mismatch acne). */
void drawDepth(GLuint vao, GLuint shader, int vertices, mat4 m) {
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
	glUniform1f(glGetUniformLocation(shader, "displace"), 0.0f);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &g_lightSpaceMatrix.m[0][0]);
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
}

void drawPlanetDepth(GLuint shader, mat4 m, float lodRadius) {
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 1);
	glUniform1f(glGetUniformLocation(shader, "displace"), 1.0f);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &g_lightSpaceMatrix.m[0][0]);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glActiveTexture(GL_TEXTURE0);
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), g_planetGen.seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), g_planetGen.oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), LAND_RELIEF);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), OCEAN_RELIEF);
	/* Same vertex lod inputs as setBodyUniforms, so the displaced caster
	   matches the drawn surface vertex for vertex. */
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*getWindowHeight()));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	QuadTree_DrawShadowCasters(m, lodRadius, g_lightSpaceMatrix);
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
	struct obj object, ship;
	float theta = 0.0;
	//printf("\tWorkdir: %s\n", getenv("PWD"));
	{
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		char *slash = strrchr(exePath, '\\');
		if(slash) *slash = '\0';
		chdir(exePath);
	}
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
	GLuint quadShader = initQuadShader();
	GLuint planetShader = initPlanetShader();
	attachGUIShaders();
	uiInit();
	uiSetupCameraSliders();
	LoadingScreen_Step(); // all shader programs
	/* The quadtree body LOD: six face grids + shared instance VBO, attribute
	   layout wired once (all three body programs declare vPosition at 0 and
	   the instance matrix at 4-7). */
	QuadTree_InitGL(quadShader);
	LoadingScreen_Step();

	initSun();
	LoadingScreen_Step();

	initSkybox();
	LoadingScreen_Step();
	GLuint skyboxTexture = loadSkyboxCube();
	LoadingScreen_Step();

	GLuint earthTex = loadTexture("../assets/earth.jpg", 0);
	LoadingScreen_Step();
	GLuint moonTex = loadTexture("../assets/moon.jpg", 0);
	LoadingScreen_Step();
	GLuint planetTex = loadTexture("../assets/europa.jpg", 0);
	LoadingScreen_Step();
	GLuint planetNorm = loadTexture("../assets/NormalMap.png", 0);
	LoadingScreen_Step();
	GLuint planetDisp = loadTexture("../assets/DisplacementMap.png", 0);
	LoadingScreen_Step();

	GLuint ringTex = loadTexture("../assets/ring.png", 1);
	LoadingScreen_Step();
	GLuint sphereVAO = initSphere();
	initAtmosphere(atmosphereShader, planet);
	GLuint ringVAO = initRing();
	LoadingScreen_Step();
	GLuint objectVAO = initObjectBuffer("../assets/rifter.obj", &ship);
	LoadingScreen_Step();
	GLuint rockVAO = initObjectBuffer("../assets/rock.obj", &object);
	LoadingScreen_Step();
	GLuint rock2VAO = initObjectBuffer("../assets/rock2.obj", &object);
	LoadingScreen_Step();
	GLuint rock3VAO = initObjectBuffer("../assets/rock3.obj", &object);
	LoadingScreen_Step();

	GLuint quadCubeVAO = initQuadCube(30);
	LoadingScreen_Step();

	GLuint depthbuffer = initDepthbuffer();
	GLuint sunFramebuffer = initFramebuffer(&sunNoiseTexture, 512, 512);
	LoadingScreen_Step();
	//GLuint quadVAO = initQuadVAO();

	/* Bake the planet height field: GPU shaping + CPU erosion + sea level
	   stats (TerrainGen.c). The seed advances on every H-key re-bake. */
	unsigned int planetSeed = 1337u;
	planetHeightTex = generatePlanetHeightmap(planetSeed, &g_planetGen, LoadingScreen_Step);

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
	/* World position of the planet centre (the scene origin). */
	dvec3 translation = {0.0, 0.0, 0.0};
	g_waveParams[0] = 1.0f;   // shell radius = sea level (planet coast is at 1.0)
	g_waveParams[1] = 0.25f;  // ~0.0006 rms: well under the land relief
	g_waveParams[2] = 1.0f;
	g_waveParams[3] = 8.0f;
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

		/* H: re-roll the seed and re-bake the height field (1-3 s hitch). */
		if (consumeRegenRequest()) {
			glDeleteTextures(1, &planetHeightTex);
			planetHeightTex = generatePlanetHeightmap(++planetSeed, &g_planetGen, NULL);
		}

		generateWaves(&ocean);

		if (!getRotationPaused())
			theta += 0.5;

		GLfloat currentFrame = glfwGetTime();
		deltaTime = currentFrame - lastFrame;
		lastFrame = currentFrame;
		doMovement(deltaTime);
		/* This frame's floating origin (see camRelTranslate). */
		g_cam = getCameraPosition();

		if(currentFrame - lastTime >= 1.0)
		{
			//printf("%f\n", fpsFrames);
			fpsFrames = 0.0;
			lastTime += 1.0;
		}

		/* Planet radius in world units. Everything planet-coupled (sun
		   distance, shadow ortho/depth divisor, sun billboard, moon orbit,
		   camera distance, surface presets) is scaled with this. */
		float fScale = 300.0;
		float fScaleFactor = 1.25;//1.025;

		/* Atmosphere (OpenGLSolar Earth params): shell 4.24% above the
		   surface (2x the original 2.12% so tall terrain stays inside),
		   Rayleigh tint C_R, solar brightness E. Shared by the dome
		   (in-scatter) and the planet/water passes (extinction). */
		float atmoInner = fScale;
		float atmoOuter = fScale * 1.0424f;
		vec3 atmoC_R = { 0.3f, 0.7f, 1.0f };
		float atmoE = 14.3f;

		//int terrainMaxLOD = (int)(log(fScale)/log(2));

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.0, 0.0, 0.0, 1.0);

		//Light calculations per frame
		dvec3 lightWorld = rotateLight(quadCubeVAO, ringShader, qc.vertexNumber, sunNoiseTexture, theta, translation);
		/* Shading and shadows both use the camera-relative light. */
		vec3 lightPositionXYZ = toCamRel(lightWorld);
		vec4 lightPosition = {lightPositionXYZ.x, lightPositionXYZ.y, lightPositionXYZ.z, 1.0};

		/* Planet orientation, shared by the shadow casters and the main pass. */
		mat4 matR = multiplymat4(rotateY(theta/5.0), rotateX(45.0));
		mat4 planetModel = multiplymat4(camRelTranslate(translation), multiplymat4(scale(fScale), matR));

		/* Shadow frustum: a perspective from the sun (a point light, matching
		   the per-pixel light direction the shaders use) whose cone is tangent
		   to the planet's bounding sphere (land relief + margin), so all
		   SHADOW_MAP_SIZE^2 texels land on the planet. Depth spans the moon's
		   orbit on either side so it can both eclipse the planet and be
		   eclipsed by it. */
		{
			double sunDist = dlengthvec3(dsubvec3(lightWorld, translation));
			double boundR = fScale * (1.0 + LAND_RELIEF) * 1.02;
			double halfAngle = asin(fmin(boundR / sunDist, 0.99));
			double reach = MOON_ORBIT + MOON_RADIUS * 1.5;
			float nearP = (float)fmax(sunDist - reach, 1.0);
			float farP = (float)(sunDist + reach);
			mat4 lightProjection = perspective((float)(2.0 * halfAngle * 180.0 / M_PI), 1.0f, nearP, farP);
			mat4 lightView = getLightViewMatrix(lightWorld, translation);
			g_lightSpaceMatrix = multiplymat4(lightProjection, lightView);
			g_shadowTexel = (float)(2.0 * sunDist * tan(halfAngle) / SHADOW_MAP_SIZE);
		}
		mat4 lightSpaceMatrix = g_lightSpaceMatrix;

		/* Shadow pass. The ring is not a caster: it is not drawn in the main
		   pass either, and the invisible ring was shadowing the planet. A
		   slope-scaled polygon offset handles the remaining grazing acne
		   together with the receiver offsets in sunVisibility. */
		glBindFramebuffer(GL_FRAMEBUFFER, depthbuffer);
			glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
			glClear(GL_DEPTH_BUFFER_BIT);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(1.5f, 2.0f);
			model = multiplymat4(multiplymat4(camRelTranslate(moonWorldPosition(theta, translation)), scale(MOON_RADIUS)),rotateX(90.0));
			drawDepth(quadCubeVAO, depthShader, qc.vertexNumber, model);
			drawPlanetDepth(depthShader, planetModel, fScale);
			glDisable(GL_POLYGON_OFFSET_FILL);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, getWindowWidth(), getWindowHeight());

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.1, 0.0, 0.2, 1.0);

		//Skybox: full-screen background, drawn first (far plane, no depth write)
		mat4 skyboxProj = perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar);
		drawSkybox(skyboxTexture, skyboxProj, getViewRotation());

		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		drawOrbit(ringShader, moonTex, theta, translation, lightPosition);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		//Planet ring
		model = multiplymat4(multiplymat4(camRelTranslate(translation), rotateX(80.0)), scale(fScale*1.5));
		//draw(ringVAO, ringShader, planetRing.vertexNumber, ringTex, planetNorm, model, translation, lightPosition, lightSpaceMatrix);
		//Planet
		model = multiplymat4(camRelTranslate(translation), scale(fScale));
		mat4 m = multiplymat4(model,matR);
		//drawTess(quadCubeVAO, tessShader, qc.vertexNumber, textureColorBuffer, dxWaveTex, planetNorm, planetDisp, m, translation, lightPosition, lightSpaceMatrix);
		//Water: drawn after the planet, shell at sea level (see drawWater)
		//Sun (billboard): opaque disc drawn with the scene; glow drawn last (below)
		mat4 sunProj = perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar);
		mat4 sunView = getViewMatrix();
		drawSunDisc(sunProj, sunView, lightPositionXYZ, SUN_GLOW_RADIUS);
		//FBO Visualizer
		//model = multiplymat4(translate(-75.0, 25.0, 0.0), scale(10.0));
		//draw(quadVAO, fboShader, 6, textureColorBuffer, planetNorm, model, translation, lightPosition, lightSpaceMatrix);
		//Object
		//The ship sits at the camera mirrored through the origin (world -cam),
		//as before.
		float modelRotationAngle = 0.0;
		model = multiplymat4(multiplymat4(camRelTranslate(dscalarvec3(g_cam, -1.0)), rotateX(modelRotationAngle)), scale(0.5));
		draw(objectVAO, ringShader, ship.vertexNumber, earthTex, planetNorm, model, lightPositionXYZ, lightPosition, lightSpaceMatrix);

		//Quadtree planet: LOD sphere built from six instanced face grids,
		//centred camera-relative (the walk runs with the camera at the origin).
		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		model = planetModel;
		mat4 viewProj = getViewProjection();

		//Planet (land + seafloor): drawn first so the translucent water can
		//blend over the seafloor.
		setBodyUniforms(quadShader, fScale, model, lightPosition,
		                0, dxWaveTex, 0, 0, 0, 0);
		setAtmosphereExtinction(quadShader, atmoInner, atmoOuter, atmoC_R);
		QuadTree_DrawSphere(model, fScale, viewProj);

		//Water: same quadtree LOD as the planet (reuses its walk), FFT-
		//displaced sea-level shell that discards over land in-shader
		//(waveHeight/Disp/Slope + tiled detail normals + skybox reflection in
		//water.frag). Alpha-blended: water.frag fades to clear in the
		//shallows from the per-pixel water depth.
		setBodyUniforms(waterShader, fScale, model, lightPosition,
		                ocean.dyTexture, ocean.detailTexture,
		                ocean.slopeTexture, ocean.dispTexture, ocean.jacobTexture, skyboxTexture);
		setAtmosphereExtinction(waterShader, atmoInner, atmoOuter, atmoC_R);
		/* Water.frag's back-face early-out is only safe from outside the
		   shell; the surface view (camera inside) must still see it. */
		double shellCamDist = dlengthvec3(dsubvec3(g_cam, translation));
		glUniform1f(glGetUniformLocation(waterShader, "outsideShell"),
		            shellCamDist > fScale*1.05 ? 1.0f : 0.0f);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		QuadTree_DrawSphere(model, fScale, viewProj);
		glDisable(GL_BLEND);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		//TEST: flat animated wave grid above the planet (debug for FFT water)
		/*{
			vec3 testPos = {0.0, 0.0, 60.0};
			mat4 testModel = translatevec3(testPos);
			drawWTest(testQuadVAO, wTestShader, &ocean, testModel, 300.0);
		}*/

		//Ring
		//glUseProgram(instanceShader);
		//drawInstanced(rockVAO, positionsVBO, instanceShader, object.vertexNumber, instancedDraws, pos1, rotations, model, scaleArray, theta, lightPosition);
		//drawInstanced(rock2VAO, positionsVBO, instanceShader, object.vertexNumber, instancedDraws, pos2, rotations, model, scaleArray, theta, lightPosition);
		//drawInstanced(rock3VAO, positionsVBO, instanceShader, object.vertexNumber, instancedDraws, pos3, rotations, model, scaleArray, theta, lightPosition);

		//Atmosphere dome: exact OpenGLSolar port (GPU Gems ch.16 scattering on the
		//scaled planet shell; params above with the planet extinction)
		drawAtmosphereDome(atmosphereShader, sunProj, sunView, camRelTranslate(translation),
		                   atmoInner, atmoOuter, atmoC_R, atmoE, lightPosition);

		//Sun corona halo — must be drawn LAST (additive, does not write depth)
		drawSunGlow(sunProj, sunView, lightPositionXYZ, SUN_GLOW_RADIUS);

		//Camera speed sliders: UI overlay, drawn over everything. Laid out
		//every frame so they track window resizes.
		{
			int ww, wh;
			glfwGetWindowSize(window, &ww, &wh);
			uiLayoutSliders(ww, wh, UI_SLIDER_COUNT);
		}
		uiDrawAll();

		glfwPollEvents();
		glfwSwapBuffers(window);
	}
	glfwTerminate();
	return 0;
}
