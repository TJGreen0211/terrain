#ifndef FRAME_H
#define FRAME_H

#include "engine/math/LinearAlg.h"

/* What every pass of one frame shares, built once per frame by the app
   before the first draw. Rendering is camera-relative: world positions stay
   in double on the CPU, the camera is subtracted in double, and only the
   small difference reaches the GPU, so the camera is the origin of every
   space a shader sees.

   Per-frame values only: state that outlives the frame belongs to the
   module that owns it. */
typedef struct FrameContext {
	dvec3 cam;          /* world camera position: this frame's floating origin */
	mat4 proj;          /* main-pass projection (infinite reversed-Z) */
	mat4 view;          /* rotation-only view */
	mat4 viewProj;      /* proj * view; the quadtree walks cull against it */
	float nearPlane;    /* main-pass near plane (m) */
	float viewportH;    /* main-pass height (px), for LOD */
	vec4 lightPos;      /* the sun, camera-relative, w = 1 */
	double time;        /* scene clock (s) */
} FrameContext;

#endif
