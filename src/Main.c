#include "Main.h"
#include "Sun.h"
#include "Skybox.h"
#include "Atmosphere.h"
#include "PbrGen.h"
#include <stdio.h>
//#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <pthread.h>

float zNear = 0.5, zFar = 100000.0;
GLuint depthMap;
struct sphere planet;
struct ring planetRing;
struct quadCube qc, qc2;

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
	depthMap = generateTextureAttachment(1, 0, getWindowWidth(), getWindowHeight());
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

mat4 getLookAtMatrix(vec4 lightPosition, vec3 lookAt) {
	mat4 rxry;
	vec3 d = {lightPosition.x - lookAt.x, lightPosition.y - lookAt.y, lightPosition.z - lookAt.z};
	d = normalizevec3(d);
	float rad = 180.0 / M_PI;
	float lightYaw = asin(-d.y) * rad;
	float lightPitch = atan2(d.x, -d.z) * rad;
	//printf("Yaw: %f, Pitch: %f\n", lightYaw, lightPitch);
	/*if(lookAt.x == 1.0 || lookAt.z == -1.0) {
		//printf("%f, %f, %f\n", lookAt.x, lookAt.y, lookAt.z);
		//rxry = multiplymat4(rotateZ(180.0), multiplymat4(rotateX(lightYaw), rotateY(lightPitch)));
		rxry = multiplymat4(rotateY(lightPitch), multiplymat4(rotateZ(180.0), rotateX(lightYaw)));
	}
	else {
	vec3 s = {-1.0, 1.0, 1.0};
		rxry = multiplymat4( scalevec3(s), multiplymat4(rotateX(lightYaw), rotateY(lightPitch)));
	}*/
	rxry = multiplymat4(rotateX(lightYaw), rotateY(lightPitch));
	return multiplymat4(rxry, translatevec4(lightPosition));
}

void initMVP(int shader, mat4 m, mat4 v) {
	mat4 p = perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &p.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &v.m[0][0]);
}

void drawInstanced(GLuint vao, GLuint vbo, GLuint shader, int vertexNumber, int drawAmount, vec3 *positions, mat4 *rotations, mat4 model, float *scaleArray, float theta, vec4 lightPosition) {
	vec4 cameraPos = getCameraPosition(model);

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

	mat4 positionMatrix = translatevec3(position);
	vec4 camPosition = getCameraPosition(positionMatrix);
	glUniform3f(glGetUniformLocation(shader, "camPosition"), camPosition.x, camPosition.y, camPosition.z);
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
	mat4 positionMatrix = translatevec3(position);
	vec4 camPosition = getCameraPosition(positionMatrix);

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), camPosition.x, camPosition.y, camPosition.z);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpaceMatrix.m[0][0]);
	glUniform1f(glGetUniformLocation(shader, "time"), glfwGetTime()/100.0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depthMap);
	glUniform1i(glGetUniformLocation(shader, "depthMap"), 1);
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

vec4 rotateLight(GLuint vao, GLuint shader, int vertices, GLuint texture, float theta, vec3 position)
{
	glUseProgram(shader);
	glBindVertexArray (vao);
	mat4 positionMatrix = translatevec3(position);
	vec4 camPosition = getCameraPosition(positionMatrix);

	vec3 translation;
	translation.x = (-4000.0);// * cos(theta/75.0);
	translation.y = 0.0;
	translation.z = (-4000.0);// * sin(theta/75.0);

	mat4 model = multiplymat4(multiplymat4(multiplymat4(positionMatrix, translatevec3(translation)), scale(15.0)),rotateX(90.0));
	initMVP(shader, model, getViewMatrix());

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), camPosition.x, camPosition.y, camPosition.z);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
	glBindVertexArray(0);

	vec4 l = {translation.x, translation.y, translation.z, 1.0};
	return l;
}

void drawOrbit(GLuint vao, GLuint shader, int vertices, GLuint texture, float theta, mat4 m, vec3 position)
{
	glUseProgram(shader);
	glBindVertexArray (vao);
	mat4 positionMatrix = translatevec3(position);
	vec4 camPosition = getCameraPosition(positionMatrix);

	vec3 translation;
	translation.x = (2000.0) * cos(theta/100.0);
	translation.y = 0.0;
	translation.z = (2000.0) * sin(theta/100.0);

	mat4 model = multiplymat4(multiplymat4(multiplymat4(positionMatrix, translatevec3(translation)), scale(100.0)),rotateX(90.0));
	initMVP(shader, model, getViewMatrix());

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), camPosition.x, camPosition.y, camPosition.z);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
}

void drawDepth(GLuint vao, GLuint shader, int vertices, vec4 lightPosition, mat4 m, mat4 l) {
	glUseProgram(shader);
	glBindVertexArray(vao);
	initMVP(shader, m, l);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &l.m[0][0]);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
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
	mat4 lightView;
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

	GLuint textureColorBuffer, sunNoiseTexture;

	struct waves ocean;
	initializeWaves(&ocean, 256);
	initializeNoise();

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

	initSun();

	initSkybox();
	GLuint skyboxTexture = loadSkyboxCube();

	GLuint earthTex = loadTexture("../assets/earth.jpg", 0);
	GLuint moonTex = loadTexture("../assets/moon.jpg", 0);
	GLuint planetTex = loadTexture("../assets/europa.jpg", 0);
	GLuint planetNorm = loadTexture("../assets/NormalMap.png", 0);
	GLuint planetDisp = loadTexture("../assets/DisplacementMap.png", 0);

	GLuint ringTex = loadTexture("../assets/ring.png", 1);
	GLuint sphereVAO = initSphere();
	initAtmosphere(atmosphereShader, planet);
	GLuint ringVAO = initRing();
	GLuint objectVAO = initObjectBuffer("../assets/rifter.obj", &ship);
	GLuint rockVAO = initObjectBuffer("../assets/rock.obj", &object);
	GLuint rock2VAO = initObjectBuffer("../assets/rock2.obj", &object);
	GLuint rock3VAO = initObjectBuffer("../assets/rock3.obj", &object);
	GLuint sphereFrontXVAO = initSubQuadX(10, 0);
	GLuint sphereBackXVAO = initSubQuadX(10, 1);
	GLuint sphereFrontYVAO = initSubQuadY(10, 0);
	GLuint sphereBackYVAO = initSubQuadY(10, 1);
	GLuint sphereFrontZVAO = initSubQuadZ(10, 0);
	GLuint sphereBackZVAO = initSubQuadZ(10, 1);

	GLuint quadCubeVAO = initQuadCube(30);

	GLuint depthbuffer = initDepthbuffer();
	GLuint framebuffer = initFramebuffer(&textureColorBuffer, 1024, 1024);
	GLuint sunFramebuffer = initFramebuffer(&sunNoiseTexture, 512, 512);
	//GLuint quadVAO = initQuadVAO();

	// Generate the planet's per-face PBR maps (albedo, height, normal,
	// roughness, metallic, AO) from the planet's noise field and write
	// them out under ../assets/planet/pbr/ for inspection
	generatePlanetPbrTextures();

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

	GLuint positionsVBO;
	glGenBuffers(1, &positionsVBO);

	GLuint quadtreeVBO;
	glGenBuffers(1, &quadtreeVBO);

	GLuint sizeVBO;
	glGenBuffers(1, &sizeVBO);

	mat4 model;
	glViewport(0, 0, getWindowWidth(), getWindowHeight());

	float deltaTime = 0.0;
	float lastFrame = 0.0;
	vec3 translation = {0.0, 0.0, 0.0};
	mat4 lightProjection = ortho(-4000.0, 4000.0, -4000.0, 4000.0, zNear, zFar);
	g_waveParams[0] = 1.01f;
	g_waveParams[1] = 0.8f;
	g_waveParams[2] = 60.0f;
	g_waveParams[3] = 8.0f;
	//mat4 lightProjection = perspective(90.0, getWindowWidth()/getWindowHeight(), zNear, zFar);

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

	while(!glfwWindowShouldClose(window))
	{
		/*if(checkShaderChange(tessShader, vertCheck, fragCheck, vertTime, fragTime)) {
			waterShader = initWaterShader();
			vertTime = getFileLastChangeTime(vertCheck);
			fragTime = getFileLastChangeTime(fragCheck);
		}*/
		fpsFrames++;

		generateWaves(&ocean);

		clock_t start,end;
		float time_spent;
		start=clock();
		//for(int i = 0; i <100;i++) {
		generateNoiseTexture(framebuffer, 0);
		end=clock();
		time_spent=(((float)end - (float)start) / 1000000.0F );
		//printf("\nSystem time is at %f\n seconds", time_spent);

		//generateNoiseTexture(sunFramebuffer, 1);
		theta += 0.5;

		GLfloat currentFrame = glfwGetTime();
		deltaTime = currentFrame - lastFrame;
		lastFrame = currentFrame;
		doMovement(deltaTime);

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

		//int terrainMaxLOD = (int)(log(fScale)/log(2));

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.0, 0.0, 0.0, 1.0);

		//Light calculations per frame
		vec4 lightPosition = rotateLight(quadCubeVAO, ringShader, qc.vertexNumber, sunNoiseTexture, theta, translation);
		vec3 lightPositionXYZ = {lightPosition.x, lightPosition.y, lightPosition.z};
		//vec4 negativeLight = {lightPosition.x, lightPosition.y, lightPosition.z, lightPosition.w};
		lightView = getLookAtMatrix(lightPosition, translation);//multiplymat4(rxry, translatevec4(negativeLight));
		mat4 lightSpaceMatrix = multiplymat4(lightProjection, lightView);
		g_lightSpaceMatrix = lightSpaceMatrix;

		glBindFramebuffer(GL_FRAMEBUFFER, depthbuffer);
			glClear(GL_DEPTH_BUFFER_BIT);
			vec3 position;
			position.x = (2000.0) * cos(theta/100.0);
			position.y = 0.0;
			position.z = (2000.0) * sin(theta/100.0);
			mat4 positionMatrix = translatevec3(translation);
			model = multiplymat4(multiplymat4(multiplymat4(positionMatrix, translatevec3(position)), scale(100.0)),rotateX(90.0));
			drawDepth(quadCubeVAO, depthShader, qc.vertexNumber, lightPosition, model, lightSpaceMatrix);
			model = multiplymat4(multiplymat4(translatevec3(translation), rotateX(80.0)), scale(fScale*1.5));
			glDisable(GL_CULL_FACE);
			drawDepth(ringVAO, depthShader, planetRing.vertexNumber, lightPosition, model, lightSpaceMatrix);
			glEnable(GL_CULL_FACE);
			model = multiplymat4(translatevec3(translation), scale(fScale));
			drawDepth(quadCubeVAO, depthShader, qc.vertexNumber, lightPosition, model, lightSpaceMatrix);
			//model = multiplymat4(translate(25.0, 0.0, -90.0), scale(10.0));
			//drawDepth(quadVAO, depthShader, 6, lightPosition, model, lightSpaceMatrix);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.1, 0.0, 0.2, 1.0);

		//Skybox: full-screen background, drawn first (far plane, no depth write)
		mat4 skyboxProj = perspective(45.0, getWindowWidth()/getWindowHeight(), zNear, zFar);
		drawSkybox(skyboxTexture, skyboxProj, getViewRotation());

		drawOrbit(quadCubeVAO, ringShader, qc.vertexNumber, moonTex, theta, model, translation);
		//Planet ring
		model = multiplymat4(multiplymat4(translatevec3(translation), rotateX(80.0)), scale(fScale*1.5));
		//draw(ringVAO, ringShader, planetRing.vertexNumber, ringTex, planetNorm, model, translation, lightPosition, lightSpaceMatrix);
		//Planet
		mat4 matR = multiplymat4(rotateY(theta/5.0), rotateX(45.0));
		model = multiplymat4(translatevec3(translation), scale(fScale));
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
		vec3 arcBallPos = getCamera();
		float modelRotationAngle = 0.0;
		model = multiplymat4(multiplymat4(translate(-arcBallPos.x, -arcBallPos.y, -arcBallPos.z), rotateX(modelRotationAngle)), scale(0.5));
		draw(objectVAO, ringShader, ship.vertexNumber, earthTex, planetNorm, model, lightPositionXYZ, lightPosition, lightSpaceMatrix);

		//Quadtree
		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		//matR = multiplymat4(rotateY(theta/5.0), rotateX(45.0));
		//model = multiplymat4(translatevec3(translation), scale(fScale));
		//mat4 m = multiplymat4(model,matR);
		model = multiplymat4(scale(fScale), matR);
		int order[3];
		float negate = 1.0;
		order[0] = 0; order[1] = 1; order[2] = 2;
		traverseQuad(model, order, sphereFrontXVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);
		negate = -1.0;
		traverseQuad(model, order, sphereBackXVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);

		negate = 1.0;
		order[0] = 0; order[1] = 2; order[2] = 1;
		traverseQuad(model, order, sphereFrontYVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);
		negate = -1.0;
		traverseQuad(model, order, sphereBackYVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);

		negate = 1.0;
		order[0] = 2; order[1] = 0; order[2] = 1;
		traverseQuad(model, order, sphereFrontZVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);
		negate = -1.0;
		traverseQuad(model, order, sphereBackZVAO, quadtreeVBO, sizeVBO, quadShader, lightPositionXYZ, lightPosition, textureColorBuffer, dxWaveTex, depthMap, 0, 0, 0, negate);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		//Water: same quadtree LOD as the planet, FFT-displaced sea-level
		//shell that discards over land in-shader (waveHeight/Dx/Dz + tiled
		//detail normals + skybox reflection in water.frag)
		order[0] = 0; order[1] = 1; order[2] = 2;
		traverseQuad(model, order, sphereFrontXVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, 1.0);
		traverseQuad(model, order, sphereBackXVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, -1.0);

		order[0] = 0; order[1] = 2; order[2] = 1;
		traverseQuad(model, order, sphereFrontYVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, 1.0);
		traverseQuad(model, order, sphereBackYVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, -1.0);

		order[0] = 2; order[1] = 0; order[2] = 1;
		traverseQuad(model, order, sphereFrontZVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, 1.0);
		traverseQuad(model, order, sphereBackZVAO, quadtreeVBO, sizeVBO, waterShader, translation, lightPosition, ocean.dyTexture, ocean.detailTexture, depthMap, ocean.dxTexture, ocean.dzTexture, skyboxTexture, -1.0);

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
		//scaled planet shell; Earth params: C_R (0.3, 0.7, 1.0), E 14.3, 2.12% shell)
		{
			vec3 C_R = { 0.3f, 0.7f, 1.0f };
			drawAtmosphereDome(atmosphereShader, sunProj, sunView, identityMatrix(),
			                   fScale, fScale * 1.0212f, C_R, 14.3f, lightPositionXYZ);
		}

		//Sun corona halo — must be drawn LAST (additive, does not write depth)
		drawSunGlow(sunProj, sunView, lightPositionXYZ, SUN_GLOW_RADIUS);

		glfwPollEvents();
		glfwSwapBuffers(window);
	}
	glfwTerminate();
	return 0;
}
