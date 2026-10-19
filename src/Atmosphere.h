#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include "Main.h"

// Build the atmosphere dome's VAO/VBO (positions followed by normals) from the
// app's tetrahedron `planet` mesh -- the same mesh the quadtree planet is drawn
// from. The shader is the one created by initAtmosphereShader() in Main.c.
void initAtmosphere(GLuint shader, sphere mesh);

// Additive sky-dome pass (Rayleigh limb tint + white Mie term), exactly the
// OpenGLSolar atmosphere: scattering math in atmosphere.frag, camera position
// recovered from modelmat * view on the CPU. No depth write; the cull face is
// swapped to GL_FRONT while the camera sits inside the outer shell.
// modelmat is the planet's transform WITHOUT the dome scale (identity here:
// the planet sits at the origin); the dome scale is fOuter.
void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, float fOuter, vec3 C_R, float E, vec3 lightPosition);

#endif
