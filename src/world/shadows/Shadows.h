#ifndef SHADOWS_H
#define SHADOWS_H

#include "engine/core/gl.h"
#include "engine/math/LinearAlg.h"

/* Cascaded sun shadows (shaders/shadow.glsl). SHADOW_CASCADES orthographic
   maps along the sun direction, each fitted around a slice of the camera's
   view distance, from the nearest visible terrain out to
   SHADOW_CASCADE_MAX_DIST_M (WorldConfig.h). The global planet-wide map
   (SHADOW_MAP_SIZE^2) stays as the fallback beyond them and for moon
   eclipses. Casters render with Shadows_DepthProgram (depth.vert).

   Everything is camera-relative double on the CPU (the camera is the
   origin); only the final matrices are downcast. */

#define SHADOW_MAX_CASCADES 4   /* = shadow.glsl's array size */

typedef struct ShadowCascade {
	mat4 lightSpace;        /* camera-relative -> clip (standard depth) */
	dvec3 center;           /* camera-relative sphere centre */
	double radius;          /* sphere radius = box half-size, m */
	double texel;           /* texel size, m */
} ShadowCascade;

typedef struct ShadowCascades {
	int count;              /* 0 = off (camera far from any terrain) */
	ShadowCascade c[SHADOW_MAX_CASCADES];
} ShadowCascades;

extern ShadowCascades g_cascades;
extern int g_shadowDebug;   /* debug panel: tint receivers by level */

/* The global map, the cascades and the caster program. */
void Shadows_InitGL(void);

/* Global map for this frame: an orthographic box of half-size boundR
   looking from eyeWorld at targetWorld, 2 * reach deep (world positions;
   cam is the frame's camera, the matrices are camera-relative). */
void Shadows_FitGlobal(dvec3 eyeWorld, dvec3 targetWorld, dvec3 cam, double boundR, double reach);
mat4 Shadows_GlobalLightSpace(void);   /* camera-relative -> clip */
float Shadows_GlobalTexel(void);        /* world size of one texel (m) */
/* Renders into the global map: binds it, sets the viewport, clears. */
void Shadows_BeginGlobal(void);

/* Caster program (depth.vert/depth.frag) and a plain, undisplaced mesh
   caster (e.g. the moon sphere) drawn with it. */
GLuint Shadows_DepthProgram(void);
void Shadows_DrawMeshCaster(GLuint vao, int vertices, mat4 model, mat4 lightSpace);

/* Fits this frame's cascades. camWorld: camera world position (double, for
   the texel snap, which must not move with the camera); forward: camera
   forward axis; sunDir: unit, toward the sun; nearest/farthest: view
   distances (m) the terrain can be seen over. */
void Shadows_Fit(dvec3 camWorld, dvec3 forward, dvec3 sunDir, double nearest, double farthest);

/* Renders into cascade i: binds its layer, sets the viewport, clears, and
   turns on depth clamping (casters up-sun of the box flatten onto its near
   plane and still cast). Shadows_EndCascades restores. */
void Shadows_BeginCascade(int i);
void Shadows_EndCascades(void);

/* Receiver uniforms (shadow.glsl) for a bound program; the cascade array is
   bound to texture unit `unit`. */
void Shadows_BindReceiver(GLuint shader, int unit);
/* Every shadow input of a receiver: the global map at `unit` (depthMap,
   lightSpace, shadowTexel), then the cascades at unit 13. */
void Shadows_BindMaps(GLuint shader, int unit);

#endif
