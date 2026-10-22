#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "engine/math/LinearAlg.h"
#include "engine/gfx/Buffers.h"

vec3 *Mesh_Tangents(int vertexNumber, vec3 *points, vec3 *tangent);
vec3 *Mesh_SmoothNormals(vec3 vna[], vec3 *vertices, vec3 *normals, int size);
vec3 *Mesh_Normals(vec3 normals[], float *vertices, int size);

#endif
