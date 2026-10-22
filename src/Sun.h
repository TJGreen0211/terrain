#ifndef SUN_H
#define SUN_H

#include <glad/glad.h>
#include "engine/math/LinearAlg.h"

// `radius` (both passes): world-space radius used to project the sun's
// on-screen size. The NDC half-size is the projected radius of a sphere this
// big at the sun's depth, so the sun shrinks/grows with the camera like a
// real sphere would. Main.c passes sun radius * SUN_GLOW_RADII (WorldConfig.h).

// Creates the sun billboard shader + fullscreen quad and loads the
// sun colour + normal textures. Call once after the GL context is ready.
void initSun(void);

// Opaque central disc. Writes depth (so it occludes geometry behind it) and
// must be drawn with the main scene (depth test on, no blending).
void drawSunDisc(mat4 proj, mat4 view, vec3 center, float radius);

// Additive corona halo. Never writes depth (so geometry in front still shows)
// and must be drawn LAST, after all scene geometry.
void drawSunGlow(mat4 proj, mat4 view, vec3 center, float radius);

#endif
