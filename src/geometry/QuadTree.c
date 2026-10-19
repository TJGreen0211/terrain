#include "Quadtree.h"

static mat4 pos[512];

/* Per-frame extras bound for every traversal (harmless no-ops for shaders
   that do not declare them). Main.c fills these each frame. */
mat4 g_lightSpaceMatrix;
float g_waveParams[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // oceanOffset, waveAmp, normalStrength, tiling

static void drawTree(GLuint vao, GLuint vbo, GLuint svbo, GLuint shader, int vertices, GLuint texture, GLuint normal, GLuint depthMap, GLuint dxTex, GLuint dzTex, GLuint skyTex, mat4 m, vec3 position, vec4 lightPosition, int drawAmount) {
	//glDisable(GL_CULL_FACE);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	//glEnable(GL_BLEND);
	//glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(shader);
	glBindVertexArray(vao);
	mat4 positionMatrix = translatevec3(position);
	vec4 camPosition = getCameraPosition(positionMatrix);

	mat4 p = perspective(45.0, getWindowWidth()/getWindowHeight(), 0.5, 100000.0);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &p.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	mat4 v = getViewMatrix();
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &v.m[0][0]);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(0));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(sizeof(vec4)));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(2*sizeof(vec4)));
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(mat4), BUFFER_OFFSET(3*sizeof(vec4)));
	glEnableVertexAttribArray(7);

	glVertexAttribDivisor(4, 1);
	glVertexAttribDivisor(5, 1);
	glVertexAttribDivisor(6, 1);
	glVertexAttribDivisor(7, 1);

	glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*512, &pos, GL_STATIC_DRAW);

	/*glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribDivisor()
	glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(float), BUFFER_OFFSET(0));
	glEnableVertexAttribArray(7);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float)*10*10*6, &pos, GL_STATIC_DRAW);*/

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), camPosition.x, camPosition.y, camPosition.z);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &g_lightSpaceMatrix.m[0][0]);
	glUniform1f(glGetUniformLocation(shader, "oceanOffset"), g_waveParams[0]);
	glUniform1f(glGetUniformLocation(shader, "waveAmp"), g_waveParams[1]);
	glUniform1f(glGetUniformLocation(shader, "waveNormalStrength"), g_waveParams[2]);
	glUniform1f(glGetUniformLocation(shader, "waveTiling"), g_waveParams[3]);
	//glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpaceMatrix.m[0][0]);
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
	glUniform1i(glGetUniformLocation(shader, "detailNormal"), 2);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, dxTex);
	glUniform1i(glGetUniformLocation(shader, "waveDx"), 3);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, dzTex);
	glUniform1i(glGetUniformLocation(shader, "waveDz"), 4);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_CUBE_MAP, skyTex);
	glUniform1i(glGetUniformLocation(shader, "skybox"), 5);
	//glDrawArrays(GL_LINES, 0, vertices);
	//glPatchParameteri(GL_PATCH_VERTICES, vertices);
	glDrawArraysInstanced(GL_TRIANGLES, 0, vertices, drawAmount);
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glActiveTexture(GL_TEXTURE0);
	//glDisable(GL_BLEND);
}

static float pointDistance(vec3 u, vec3 v) {
	return sqrt((v.x - u.x)*(v.x - u.x) + (v.y - u.y)*(v.y - u.y) + (v.z - u.z)*(v.z - u.z));
}

/* Keep the CPU matrix layout consistent with the other row-vector transforms:
   OpenGL reads the row-major data as columns, so translate * scale offsets
   the vertex before the shader projects it onto the sphere. */
static mat4 instanceMatrix(vec3 t, vec3 s) {
	return multiplymat4(translatevec3(t), scalevec3(s));
}

/* Face axes for the per-face order[] from Main.c. The baked grids are NOT on
   the axis their name suggests: initSubQuadX builds on z=-1, initSubQuadY on
   y=+1, initSubQuadZ on x=+1. order[] says where the 2D coords (values[0],
   values[1]) and the constant axis (values[2]) land: axis i carries values[order[i]].
   So n = axis holding values[2], a = axis holding values[0], b = axis holding
   values[1], and plane is the sign of the baked constant coordinate. */
static void faceAxes(const int *order, int *n, int *a, int *b, float *plane) {
	for(int i = 0; i < 3; i++) {
		if(order[i] == 2) *n = i;
		else if(order[i] == 0) *a = i;
		else *b = i;
	}
	/* X grid (order {0,1,2}) sits at z=-1; Y ({0,2,1}) at y=+1; Z ({2,0,1}) at x=+1. */
	*plane = (order[0] == 0 && order[1] == 1) ? -1.0 : 1.0;
}

static void setAxis(vec3 *v, int axis, float value) {
	if(axis == 0) v->x = value;
	else if(axis == 1) v->y = value;
	else v->z = value;
}

/* World-space point on the rendered sphere for a node: model * normalize(plocal),
   matching quad.vert (instance-transform, normalize to unit sphere, then model).
   Rows written explicitly: multiplymat4vec4 has an in-place clobbering defect. */
static vec3 sphereCenter(mat4 model, vec3 plocal) {
	vec3 pdir = normalizevec3(plocal);
	vec3 c;
	c.x = model.m[0][0]*pdir.x + model.m[0][1]*pdir.y + model.m[0][2]*pdir.z + model.m[0][3];
	c.y = model.m[1][0]*pdir.x + model.m[1][1]*pdir.y + model.m[1][2]*pdir.z + model.m[1][3];
	c.z = model.m[2][0]*pdir.x + model.m[2][1]*pdir.y + model.m[2][2]*pdir.z + model.m[2][3];
	return c;
}

void traverseQuad(mat4 model, int *order, GLuint VAO, GLuint VBO, GLuint sVBO, GLuint shader, vec3 position, vec4 lightPosition, GLuint texture1, GLuint texture2, GLuint depthMap, GLuint dxTex, GLuint dzTex, GLuint skyTex, float negate) {
	int globalCount = 0;
	float qScale = 1.0;

	int n, a, b;
	float plane;
	faceAxes(order, &n, &a, &b, &plane);

	struct qTreeNode qq[512];

	// Root node: the full face. nw stores the node's 2D center on the face;
	// its LOD point is the face center pushed onto the rendered sphere.
	qq[0].nw.x = 0.0; qq[0].nw.y = 0.0; qq[0].nw.z = 0.0;
	qq[0].width = 2.0;
	vec3 rootLocal = {0.0, 0.0, 0.0};
	setAxis(&rootLocal, n, negate*plane);
	qq[0].center = sphereCenter(model, rootLocal);

	vec3 rootT = {0.0, 0.0, 0.0};
	vec3 rootS = {1.0, 1.0, 1.0};
	setAxis(&rootS, n, negate);
	pos[globalCount] = instanceMatrix(rootT, rootS);

	int numNodes = 1;
	float width = 2.0;

	vec4 camToWorld = getCameraPosition(identityMatrix());
	vec3 v3CamToWorld = {camToWorld.x, camToWorld.y, camToWorld.z};
	float splitLOD = 16.0;
	int terrainMaxLOD = (int)(log(splitLOD)/log(2));
	float level = 128.0;
	int lastNodeCount = 0;

	for(int i = 0; i < terrainMaxLOD; i++) {
		width = qScale;
		qScale /= 2.0;

		for(int k = lastNodeCount; k < numNodes; k++) {
			if(pointDistance(qq[k].center, v3CamToWorld) < level) {
				if(globalCount + 4 >= 512)
					break;
				pos[k] = scale(0.0);
				for(int j = 0; j < 4; j++) {
					globalCount++;

					// Child 2D centers: (+,+), (-,+), (-,-), (+,-) around the parent.
					float ox = (j == 0 || j == 3) ? qScale : -qScale;
					float oy = (j == 0 || j == 1) ? qScale : -qScale;
					float cx = qq[k].nw.x + ox;
					float cy = qq[k].nw.y + oy;

					// Instance: shrink in-plane axes to the child size, keep the
					// face-normal axis at +/-1, shift the child onto its quadrant.
					vec3 t = {0.0, 0.0, 0.0};
					setAxis(&t, a, cx); setAxis(&t, b, cy);
					vec3 s = {qScale, qScale, qScale};
					setAxis(&s, n, negate);
					pos[globalCount] = instanceMatrix(t, s);

					qq[globalCount].nw.x = cx; qq[globalCount].nw.y = cy; qq[globalCount].nw.z = 0.0;
					qq[globalCount].width = width;

					// LOD point: the child's center on the face, projected to the sphere.
					vec3 plocal = {0.0, 0.0, 0.0};
					setAxis(&plocal, a, cx); setAxis(&plocal, b, cy); setAxis(&plocal, n, negate*plane);
					qq[globalCount].center = sphereCenter(model, plocal);
				}
			}
		}
		lastNodeCount = numNodes;
		numNodes = globalCount+1;
		level /= 2.0;
	}
	drawTree(VAO, VBO, sVBO, shader, 10*10*6, texture1, texture2, depthMap, dxTex, dzTex, skyTex, model, position, lightPosition, globalCount+1);
}
