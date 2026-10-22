#ifndef BUFFERS_H
#define BUFFERS_H
#include "engine/core/gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "engine/math/LinearAlg.h"

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

GLuint initBuffers(vec3 *vertices, int vertSize, vec3 *normals, int normSize, vec2 *texCoords, int texSize);
GLuint initBufferTangents(vec3 *vertices, int vertSize, vec3 *normals, int normSize, vec3 *tangents, int tanSize, vec2 *texCoords, int texSize);

#endif
