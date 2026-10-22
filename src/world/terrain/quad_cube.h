#ifndef QUAD_CUBE_H
#define QUAD_CUBE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "engine/math/linalg.h"

typedef struct quadCube {
	int size;
	int nsize;
	vec3 *points;
	vec3 *normals;
	int vertexNumber;
} quadCube;

void QuadCube_Create(int divisions, quadCube *newQuadCube);

#endif