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

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

typedef struct qTreeNode {
	vec3 center;
	vec3 nw;
	float width;
} qTreeNode;

/* Build the six cube-face grids (10x10 cells each) and the shared instance
   matrix VBO, wiring the instance-matrix vertex attributes (locations 4-7)
   into every face VAO. `shader` must be the planet program whose vertex
   shader declares `layout(location = 4) in mat4 vInstanceMatrix` and the
   `instanced` uniform. */
void QuadTree_InitGL(GLuint shader);

/* Draw one planet-sized LOD sphere centred at the origin of `model`: each of
   the six face grids is subdivided as a quadtree around the camera (nodes
   closer than the halving split distance refine; only leaves are drawn) and
   drawn with one instanced call per face. Nodes outside the `viewProj`
   frustum or past the body's horizon are culled and never refined. LOD split
   distance scales with `radius` so every body gets the same screen-space
   detail. Back-to-back calls with identical inputs reuse the last walk. */
void QuadTree_DrawSphere(mat4 model, float radius, mat4 viewProj);

void QuadTree_Shutdown(void);

#endif
