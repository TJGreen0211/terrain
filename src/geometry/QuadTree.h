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

/* Cube-sphere quadtree LOD bodies (planet, water shell, moon).

   `model` everywhere is the body's CAMERA-RELATIVE model matrix in double
   (Body_ModelMatrix: translate(position - camera) * scale(radius) *
   rotation). The walk computes each patch's four corners on the sphere from
   it in double, so the vertex shaders (shaders/patch.glsl) only ever handle
   small, camera-relative numbers. */

/* Build the shared base grid and the patch instance buffer. Every body
   program's vertex shader includes shaders/patch.glsl (vPosition at
   location 0, patch instance attributes at 4-10). */
void QuadTree_InitGL(void);

/* Draw one LOD sphere with the currently bound program: each cube face is
   subdivided as a quadtree around the camera (nodes closer than the halving
   split distance refine; only leaves are drawn), in one instanced draw.
   Nodes outside the `viewProj` frustum or past the body's horizon are culled
   and never refined. LOD split distance scales with `radius` so every body
   gets the same screen-space detail. Back-to-back calls with identical
   inputs reuse the last walk. */
void QuadTree_DrawSphere(const dmat4 *model, double radius, mat4 viewProj);

/* Shadow-map caster pass for the same body: identical camera-centred LOD
   (caster mesh == drawn mesh), culled against the light's projection * view
   instead of the camera frustum and without the camera horizon cull. */
void QuadTree_DrawShadowCasters(const dmat4 *model, double radius, mat4 lightViewProj);

/* Debug (panel toggle): 1 = place vertices with the old float path, for
   comparing against the double-precision patch corners. */
extern int g_qtLegacyPath;

void QuadTree_Shutdown(void);

#endif
