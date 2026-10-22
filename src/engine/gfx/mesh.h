#ifndef MESH_H
#define MESH_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "engine/math/linalg.h"
#include "engine/gfx/buffer.h"

vec3 *Mesh_Tangents(int vertexNumber, vec3 *points, vec3 *tangent);
vec3 *Mesh_SmoothNormals(vec3 vna[], vec3 *vertices, vec3 *normals, int size);
vec3 *Mesh_Normals(vec3 normals[], float *vertices, int size);

typedef struct sphere {
	int size;
	int nsize;
	vec3 *points;
	vec3 *normals;
	int vertexNumber;
} sphere;

sphere Mesh_Tetrahedron(int count, sphere *s);

typedef struct obj{
	vec3 *points;
	vec3 *normals;
	int vertexNumber;
	int size;
	int nsize;
} obj;

obj Mesh_LoadObj(char *fname);

#endif
