#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include "Main.h"

// Build the atmosphere dome's VAO/VBO (positions followed by normals) from the
// app's tetrahedron `planet` mesh -- the same mesh the quadtree planet is drawn
// from. The shader is the one created by initAtmosphereShader() in Main.c.
void initAtmosphere(GLuint shader, sphere mesh);

// Additive sky-dome pass (Rayleigh limb tint + white Mie term), exactly the
// OpenGLSolar atmosphere: scattering math in atmosphere.frag, camera position
// recovered from modelmat * view on the CPU. No depth write; while the camera
// sits inside the outer shell the cull face swaps to GL_FRONT and depth testing
// is disabled, so the far hemisphere still blends its (surface-clipped)
// in-scatter over the planet. The planet/water passes apply the matching
// out-scatter extinction (setAtmosphereExtinction) so fog is continuous across
// the shell boundary.
// modelmat is the planet's transform WITHOUT the dome scale (identity here:
// the planet sits at the origin); the dome scale is fOuter. lightPosition is
// world space; w != 0 -> positional, w == 0 -> directional.
void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, float fOuter, vec3 C_R, float E, vec4 lightPosition);

// Surface-side extinction uniforms for a body program (quad.frag/water.frag):
// the surface is dimmed by the transmittance along the surface-to-camera path
// through the same shell the dome scatters in. The program must be bound.
// Pass fOuter <= fInner to switch extinction off.
void setAtmosphereExtinction(GLuint shader, float fInner, float fOuter, vec3 C_R);

#endif
