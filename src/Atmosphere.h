#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include "Main.h"

// Build the atmosphere dome's position-only VAO/VBO from the app's tetrahedron
// `planet` mesh -- the same mesh the quadtree planet is drawn from. The shader
// is the one created by initAtmosphereShader() in Main.c.
void initAtmosphere(GLuint shader, sphere mesh);

// Set the shared shell parameters used to attenuate planet and water surface
// radiance along the camera-to-fragment path.
void setAtmosphereSurfaceUniforms(GLuint shader, float fInner, float fOuter, vec3 C_R);

// Additive sky-dome pass (Rayleigh limb tint + white Mie term), with the
// camera position recovered from modelmat * view on the CPU. No depth write;
// while inside the outer shell, the cull face is swapped and depth testing is
// disabled so far-side scattering can blend over the planet surface.
// modelmat is the planet's transform WITHOUT the dome scale (identity here:
// the planet sits at the origin); the dome scale is fOuter.
void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, float fOuter, vec3 C_R, float E, vec3 lightPosition);

#endif
