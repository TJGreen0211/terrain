#ifndef SCENE_TARGET_H
#define SCENE_TARGET_H
#include "engine/core/gl.h"

/* The main pass's off-screen target: multisampled RGBA16F colour and
   DEPTH32F depth, both multisample textures (fixed sample locations), so
   passes can read the depth and the resolve can read each colour sample.
   Resolved into the window by a shader (SceneTarget_Resolve).

   The colour holds linear radiance; the resolve tone-maps it (tone_map.h,
   docs/hdr.md). */

void SceneTarget_Init(int samples);   /* after GL is up */
/* (Re)allocates for a w x h scene; returns 1 when the storage changed. */
int SceneTarget_Ensure(int w, int h);
GLuint SceneTarget_Framebuffer(void);
GLuint SceneTarget_DepthTexture(void);
int SceneTarget_Samples(void);
/* Resolves into the currently bound draw framebuffer over a w x h
   viewport: each sample tone-mapped and graded, then averaged and
   dithered (resolve.frag). Leaves blend, depth test and cull as it found
   them. */
void SceneTarget_Resolve(int w, int h);

#endif
