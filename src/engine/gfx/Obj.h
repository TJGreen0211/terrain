#ifndef OBJ_H
#define OBJ_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "engine/math/LinearAlg.h"

typedef struct obj{
	vec3 *points;
	vec3 *normals;
	int vertexNumber;
	int size;
	int nsize;
} obj;

obj Mesh_LoadObj(char *fname);

#endif