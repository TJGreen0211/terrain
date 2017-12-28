#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "LinearAlg.h"
#include "Buffers.h"

typedef struct ring {
	int size;
	int nsize;
	int texsize;
	vec3 *points;
	vec3 *normals;
	vec2 *texCoords;
	int vertexNumber;
} ring;

ring createRing(int numDivides, float innerRad, float outerRad);
vec3 *generateTangents(int vertexNumber, vec3 *points, vec3 *tangent);
vec3 *generateSmoothNormals(vec3 vna[], vec3 *vertices, vec3 *normals, int size);
vec3 *generateNormals(vec3 normals[], float *vertices, int size);
unsigned int initQuadVAO();
unsigned int initSubQuadX(int divisions, int reverseOrder);
unsigned int initSubQuadY(int divisions, int reverseOrder);
unsigned int initSubQuadZ(int divisions, int reverseOrder);

#endif
