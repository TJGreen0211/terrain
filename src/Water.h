#ifndef WATER_H
#define WATER_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "LinearAlg.h"

/* FFT ocean, simulated on the GPU (Water.c): the textures below are render
   targets refilled by generateWaves. */
typedef struct waves {
	int dimension;
	float heightScale;
	unsigned int dyTexture;     // R32F   height
	unsigned int dispTexture;   // RG32F  Dx, Dz            (height units)
	/* RGBA32F dh/dx, dh/dz, (dh/dx)^2, (dh/dz)^2 (per patch length).
	   Mipmapped: a mip texel's mean square minus the square of its mean
	   slope is the slope variance that texel averaged away, which water.frag
	   turns into BRDF roughness (Bruneton 2010). One texture so the
	   roughness costs no extra upload, mip build or fetch. */
	unsigned int slopeTexture;
	unsigned int jacobTexture;  // RGBA32F dDx/dx, dDz/dz, dDx/dz, unused (per patch length)
	unsigned int detailTexture;
	/* Mean squared slope of one detailTexture layer (decoded rg, either
	   axis), so water.frag can return the variance its mips filter out. */
	float detailVar;
} waves;

/* Re-simulate the ocean every Nth generateWaves call. 1 = every frame.
   Larger values save the GPU passes but the waves visibly step (e.g. 15 at
   60 fps = 4 updates/s), since frames in between are not interpolated. */
#define WAVE_UPDATE_INTERVAL 1

/* Live wave multipliers (Main.c), uploaded every frame: [0] oceanOffset
   (shell radius, keep 1.0 = sea level), [1] waveAmp, [2] normalStrength,
   [3] tiling, [4] choppiness. Edited from the debug panel. */
extern float g_waveParams[5];

/* rms of the calibrated FFT height field, in body radii, at wave height
   1.0 (initializeWaves). water.vert rescales the geometric displacement
   from it to OCEAN_SWELL_RMS_M (WorldConfig.h). */
#define OCEAN_FFT_RMS 0.0025f

void generateWaves(waves *water);
void initializeWaves(waves *water, int dim);

#endif
