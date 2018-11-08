#ifndef QUADTREE_H
#define QUADTREE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "LinearAlg.h"
#include <windows.h>
#include <glad/glad.h>
#include <Camera.h>
#include <Window.h>

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

typedef struct qTreeNode {
	vec3 center;
	vec3 nw;
	float width;
} qTreeNode;

typedef struct qTree {
	qTreeNode *root;
	unsigned int length;
} qTree;

void traverseQuad(mat4 model, int *order, GLuint VAO, GLuint VBO, GLuint sVBO, GLuint shader, vec3 position, vec4 lightPosition, GLuint texture1, GLuint texture2, GLuint depthMap, float negate);

#endif
