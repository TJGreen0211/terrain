#ifndef WATER_H
#define WATER_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "engine/math/LinearAlg.h"
#include "WorldConfig.h"

/* FFT ocean, simulated on the GPU (Water.c): the textures below are render
   targets refilled by generateWaves. Each is a 2D array with one layer per
   cascade (OCEAN_CASCADES, WorldConfig.h), all mipmapped. */
typedef struct waves {
	int dimension;
	float heightScale;
	float patchM[OCEAN_CASCADES]; // side of each cascade's patch (m)
	unsigned int dyTexture;     // R32F   height
	unsigned int dispTexture;   // RG32F  Dx, Dz            (height units)
	/* RGBA32F dh/dx, dh/dz, (dh/dx)^2, (dh/dz)^2 (per patch length).
	   Mipmapped: a mip texel's mean square minus the square of its mean
	   slope is the slope variance that texel averaged away, which water.frag
	   turns into BRDF roughness (Bruneton 2010). One texture so the
	   roughness costs no extra upload, mip build or fetch. */
	unsigned int slopeTexture;
	unsigned int jacobTexture;  // RGBA32F dDx/dx, dDz/dz, dDx/dz, unused (per patch length)
} waves;

/* Re-simulate the ocean every Nth generateWaves call. 1 = every frame.
   Larger values save the GPU passes but the waves visibly step (e.g. 15 at
   60 fps = 4 updates/s), since frames in between are not interpolated. */
#define WAVE_UPDATE_INTERVAL 1

/* Live wave multipliers (Main.c), uploaded every frame: [0] oceanOffset
   (shell radius, keep 1.0 = sea level), [1] waveAmp, [2] normalStrength,
   [3] wave scale, [4] choppiness. Edited from the debug panel. */
extern float g_waveParams[5];

/* rms of the calibrated FFT height field (all cascades), in body radii, at wave height
   1.0 (initializeWaves). water.vert rescales the geometric displacement
   from it to OCEAN_SWELL_RMS_M (WorldConfig.h). */
#define OCEAN_FFT_RMS 0.0025f

void generateWaves(waves *water);
/* Side (m) of cascade c's patch: OCEAN_WAVE_PATCH_M / OCEAN_CASCADE_RATIO^c. */
double waveCascadePatchM(int c);
/* How much larger than true size cascade c is drawn: the "Wave scale" knob
   (g_waveParams[3]) for all but the finest, which stays at true size so
   the close-up chop keeps its detail. Lengths and heights scale by it,
   time by its square root (Froude scaling). */
double waveCascadeScale(int c);
/* Cascade c's share of the total rms height at true size (0..1, the
   cascades' variances add up to 1). */
double waveCascadeRmsFrac(int c);
void initializeWaves(waves *water, int dim);

#endif
