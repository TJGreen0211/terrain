#include "Quadtree.h"

static mat4 pos[512];

static void drawTree(GLuint vao, GLuint vbo, GLuint shader, int vertices, GLuint texture, GLuint normal, GLuint depthMap, mat4 m, vec3 position, vec4 lightPosition, int drawAmount) {
	glDisable(GL_CULL_FACE);
	//glEnable(GL_CULL_FACE);
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

	glBufferData(GL_ARRAY_BUFFER, sizeof(mat4)*10*10*6, &pos, GL_STATIC_DRAW);

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), camPosition.x, camPosition.y, camPosition.z);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
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
	//glDrawArrays(GL_LINES, 0, vertices);
	glDrawArraysInstanced(GL_TRIANGLES, 0, vertices, drawAmount);
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
	//glCullFace(GL_BACK);
	//glDisable(GL_BLEND);
}

static float pointDistance(vec3 u, vec3 v) {
	return sqrt((v.x - u.x)*(v.x - u.x) + (v.y - u.y)*(v.y - u.y) + (v.z - u.z)*(v.z - u.z));
}

void traverseQuad(int *order, GLuint VAO, GLuint VBO, GLuint shader, vec3 position, vec4 lightPosition, GLuint texture1, GLuint texture2, GLuint depthMap, float negate) {
	//Flat quad
	float scaleSize = 10.0;
	int globalCount = 0;
	mat4 model;
	float qScale = 1.0;
	float quadrant = 0.0;

	//int order[3] = {0, 1, 2};
	float values[3];
	float scaleVals[3];
	float centerVals[3];

	struct qTreeNode qq[512];
	pos[globalCount] = multiplymat4(scalevec4(qScale, qScale, 1.0), translate(quadrant, quadrant, 0.0));
	model = scale(scaleSize);
	qq[globalCount].nw.x = qScale; qq[globalCount].nw.y = qScale; qq[globalCount].nw.z = 0.0;
	qq[globalCount].width = 2.0;
	qq[globalCount].center.x = 0.0; qq[globalCount].center.y = 0.0; qq[globalCount].center.z = 0.0;

	int numNodes = 1;

	float width = 2.0;
	float x0 = 0.0;
	float y0 = 0.0;

	vec4 camToWorld = getCameraPosition(identityMatrix());
	vec3 v3CamToWorld = {camToWorld.x, camToWorld.y, camToWorld.z};
	float splitLOD = 16.0;
	int terrainMaxLOD = (int)(log(splitLOD)/log(2));
	float level = 128.0;
	int lastNodeCount = 0;

	for(int i = 0; i < terrainMaxLOD; i++) {
		//printf("i:%d\n", i);
		width = qScale;
		qScale /= 2.0;

		for(int k = lastNodeCount; k < numNodes; k++) {
			//printf("x0: %f, y0 %f\n", x0, y0);
			//printf("globalCount: %d, lastNodeCount: %d, numNodes: %d, k %d\n", globalCount, lastNodeCount, numNodes, k);
			vec2 c = {qScale, qScale};
			if(pointDistance(qq[k].center, v3CamToWorld) < level) {
				for(int j = 0; j < 4; j++) {
					globalCount++;

					values[0] = c.x+x0; values[1] = c.y+y0; values[2] = 0.0;
					scaleVals[0] = qScale; scaleVals[1] = qScale; scaleVals[2] = negate*1.0;

					pos[globalCount] = multiplymat4(translate(values[order[0]], values[order[1]], values[order[2]]), scalevec4(scaleVals[order[0]], scaleVals[order[1]], scaleVals[order[2]]));
					//printf("j:%d, c.x+x0: %f, c.y+y0: %f\n", j, c.x+x0, c.y+y0);
					model = scale(scaleSize);

					qq[globalCount].nw.x = values[order[0]]; qq[globalCount].nw.y = values[order[1]]; qq[globalCount].nw.z = values[order[2]];
					qq[globalCount].width = width;
					qq[globalCount].center.x = (c.x+x0 - qScale); qq[globalCount].center.y = (c.y+y0 - qScale); qq[globalCount].center.z = 0.0;
					vec3 cc = normalizevec3(qq[globalCount].center);
					centerVals[0] = cc.x*scaleSize; centerVals[1] = c.y*scaleSize; centerVals[2] = -scaleSize;
					qq[globalCount].center.x = centerVals[order[0]]; qq[globalCount].center.y = centerVals[order[1]]; qq[globalCount].center.z = centerVals[order[2]];


					c.y = c.x == qScale ? c.y : c.y - width;
					c.x = c.y == qScale ? c.x - width : c.x;
					if(j == 2) {c.x = qScale; c.y = -qScale;}
				}
			}
			//printf("k:%d, qq[%d].nw.x: %f, y: %f, z: %f\n", k, k, qq[k].nw.x, qq[k].nw.y, qq[k].nw.z);
			x0 = qq[k+1].nw.x;
			y0 = qq[k+1].nw.y;
		}
		lastNodeCount = numNodes;
		numNodes += globalCount-lastNodeCount+1;
		//x0 = qq[2].nw.x;
		//y0 = qq[2].nw.y;
		level /= 3.0;
	}
	//printf("globalCount %d\n", globalCount);
	drawTree(VAO, VBO, shader, 10*10*6, texture1, texture2, depthMap, model, position, lightPosition, globalCount+1);
}
