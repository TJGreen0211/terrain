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
   location 0, patch instance attributes at 4-11). */
void QuadTree_InitGL(void);

/* Height range a body's patches can reach over a spherical cap, as offsets
   from the unit sphere in body radii (every vertex the drawing program
   places there, skirts excluded). lo > hi means nothing is drawn there at
   all, and the node is culled. dir: body-local unit cap centre; capAngle:
   radians; cell: the patch half-width in cube units (its height-map mip);
   maxDist: farthest camera distance to the cap's sea-level surface, metres
   (the pixel-footprint mip). */
typedef void (*QuadTree_ReliefFn)(const double dir[3], double capAngle, double cell,
                                  double maxDist, double *lo, double *hi);

typedef struct QuadTree_Body {
	dmat4 model;              /* camera-relative, Body_ModelMatrix */
	double radius;            /* metres */
	QuadTree_ReliefFn relief; /* NULL = smooth sphere */
	double occluderRelief;    /* lowest SOLID surface, body radii (<= 0): the horizon occluder */
	double trianglePx;        /* LOD target (px per triangle edge); 0 = QT_TARGET_TRIANGLE_PX */
	/* Terrain detail the program adds on top of `relief` where it is above
	   0 (land), never pushing it below 0 (TerrainDetail.c). The walk widens
	   such bounds by detailAmp (body radii) and deepens skirts by
	   detailSeam x the patch width. 0 = none. */
	double detailAmp;
	double detailSeam;
} QuadTree_Body;

/* Draw one LOD sphere with the currently bound program: each cube face is
   subdivided as a quadtree around the camera, in one instanced draw. A node
   splits while the camera is closer than K node widths to it (its height
   range included; K from QT_TARGET_TRIANGLE_PX, WorldConfig.h), down to
   QT_MIN_VERTEX_SPACING_M. Nodes outside the `viewProj` frustum or below
   the body's horizon are culled and never refined, using per-node height
   bounds. Back-to-back calls with identical inputs reuse the last walk. */
void QuadTree_DrawSphere(const QuadTree_Body *body, mat4 viewProj);

/* Shadow-map caster pass for the same body: a coarser camera-centred LOD
   (QT_SHADOW_TRIANGLE_PX), culled against the light's projection * view
   instead of the camera frustum and without the camera horizon cull, and
   no finer than minSpacing (m) between vertices: detail below a shadow
   texel only costs time. */
void QuadTree_DrawShadowCasters(const QuadTree_Body *body, mat4 lightViewProj, double minSpacing);

/* The split rule's K for a LOD target of trianglePx (px per triangle edge)
   at the current window height: a node of width W splits while the camera
   is closer than K * W, so a leaf at distance d is at most d / K wide. */
double QuadTree_SplitK(double trianglePx);

/* Once per frame before any draw: drops the cached walk (so per-frame
   inputs the cache key cannot see, like live relief edits, apply). */
void QuadTree_BeginFrame(void);

/* Totals over every walk since QuadTree_BeginFrame (shadow casters, moon,
   planet, water), for the debug panel. */
typedef struct QuadTree_Stats {
	int walks;       /* walks run (cached redraws excluded) */
	int leaves;      /* drawn patches */
	int nodes;       /* nodes visited */
	int maxLevel;    /* deepest drawn level */
	double walkMs;   /* CPU time of the walks */
} QuadTree_Stats;
extern QuadTree_Stats g_qtStats;

/* Debug (panel toggle): 1 = place vertices with the old float path, for
   comparing against the double-precision patch corners. */
extern int g_qtLegacyPath;

void QuadTree_Shutdown(void);

#endif
