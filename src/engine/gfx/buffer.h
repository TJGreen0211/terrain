#ifndef BUFFER_H
#define BUFFER_H
#include "engine/core/gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "engine/math/linalg.h"

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

GLuint Buffer_CreateMesh(vec3 *vertices, int vertSize, vec3 *normals, int normSize, vec2 *texCoords, int texSize);
GLuint Buffer_CreateMeshTangents(vec3 *vertices, int vertSize, vec3 *normals, int normSize, vec3 *tangents, int tanSize, vec2 *texCoords, int texSize);

#endif
