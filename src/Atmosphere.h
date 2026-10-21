#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include "Main.h"

// Scattering parameters shared by the dome (in-scatter) and the planet/water
// passes (extinction, water's sky reflection). Live: edited from the debug
// panel and uploaded every frame.
typedef struct atmosphereParams {
	float shellHeight; // metres from sea level to the top of the air (0 = no atmosphere)
	float scaleHeight; // metres per e-fold of density
	vec3 C_R;         // Rayleigh tint: per-channel weight, sets the limb hue
	float E;          // solar brightness
	float K_R;        // Rayleigh constant (> thicker, bluer)
	float K_M;        // Mie constant (> thicker, hazier, whiter)
	float G_M;        // Mie phase asymmetry, -0.75..-0.999 (closer to -1: tighter sun halo)
} atmosphereParams;

extern atmosphereParams g_atmosphere;
atmosphereParams atmosphereDefaults(void); // OpenGLSolar Earth values

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
// The in-scatter along each view ray stops at the visible surface, read from
// the scene's depth (Atmosphere_SetSceneDepth), so terrain standing above sea
// level is not fogged by air behind it. It is added times (1 - the target's
// alpha): the cloud cover Clouds_Draw leaves there (0 without clouds).
void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, const atmosphereParams *atmo, vec4 lightPosition);

// The main pass's depth: a multisampled reversed-Z infinite depth texture
// (depth = near / view z) with `samples` samples. Call when it is (re)created.
void Atmosphere_SetSceneDepth(GLuint depthTex, int samples, float nearPlane);

// Surface-side extinction uniforms for a body program (quad.frag/water.frag):
// the surface is dimmed by the transmittance along the surface-to-camera path
// through the same shell the dome scatters in. The program must be bound.
// shellHeight <= 0 switches extinction off.
void setAtmosphereExtinction(GLuint shader, float fInner, const atmosphereParams *atmo);

#endif
