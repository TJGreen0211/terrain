#ifndef WATER_H
#define WATER_H
#include "engine/core/gl.h"

#include "engine/math/LinearAlg.h"
#include "world/WorldConfig.h"

/* FFT ocean, simulated on the GPU (Water.c): the textures below are render
   targets refilled by Water_Generate. Each is a 2D array with one layer per
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

/* Re-simulate the ocean every Nth Water_Generate call. 1 = every frame.
   Larger values save the GPU passes but the waves visibly step (e.g. 15 at
   60 fps = 4 updates/s), since frames in between are not interpolated. */
#define WAVE_UPDATE_INTERVAL 1

/* Live wave multipliers (defaults set by the app), uploaded every frame:
   [0] oceanOffset (shell radius, keep 1.0 = sea level), [1] waveAmp,
   [2] normalStrength, [3] wave scale, [4] choppiness. Edited from the debug
   panel. Returns the 5-float array. */
float *Water_Params(void);

/* rms of the calibrated FFT height field (all cascades), in body radii, at wave height
   1.0 (Water_Init). water.vert rescales the geometric displacement
   from it to OCEAN_SWELL_RMS_M (WorldConfig.h). */
#define OCEAN_FFT_RMS 0.0025f
/* Shore calm, in rms of each cascade's drawn height: its waves fade out
   from this water depth to the coastline (seastate.glsl). */
#define WAVE_SHORE_RMS 4.0

/* time: the scene clock in seconds (the app's App_Time). */
void Water_Generate(waves *water, double time);
/* Side (m) of cascade c's patch: OCEAN_WAVE_PATCH_M / OCEAN_CASCADE_RATIO^c. */
double Water_CascadePatchM(int c);
/* How much larger than true size cascade c is drawn: the "Wave scale" knob
   (Water_Params()[3]) for all but the finest, which stays at true size so
   the close-up chop keeps its detail. Lengths and heights scale by it,
   time by its square root (Froude scaling). */
double Water_CascadeScale(int c);
/* Cascade c's share of the total rms height at true size (0..1, the
   cascades' variances add up to 1). */
double Water_CascadeRmsFrac(int c);
void Water_Init(waves *water, int dim, double time);

struct nk_context;
/* The Water tab of the debug panel (Water.c); the startup hook snapshots
   what "Reset water" restores. */
void Water_DebugPanel(struct nk_context *ctx);
void Water_DebugPanelStartup(void);

#endif
