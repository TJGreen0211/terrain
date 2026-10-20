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
#include "ComplexMath.h"

/* Each complex grid packs two real fields as a + i*b (both spectra are
   Hermitian), so one inverse FFT yields a in .real and b in .im. */
typedef struct waves {
	complexType *hDxz;   // height + i * dDx/dz
	complexType *disp;   // Dx + i * Dz          (choppy displacement)
	complexType *slope;  // dh/dx + i * dh/dz
	complexType *jacob;  // dDx/dx + i * dDz/dz
	complexType *tildeh0k;
	complexType *conjTildeh0MK;
	int dimension;
	float heightScale;
	float *dyData;
	float *dispData;
	float *slopeData;
	float *jacobData;
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
   Larger values save the FFT + upload cost but the waves visibly step
   (e.g. 15 at 60 fps = 4 updates/s), since frames in between are not
   interpolated. */
#define WAVE_UPDATE_INTERVAL 1

/* 1 = simulate the ocean on the GPU (wavespectrum/wavefft/wavepack
   shaders, no per-frame upload), 0 = the CPU (/openmp) path. Both fill the
   same textures; flip it to compare performance. */
#define WAVES_ON_GPU 1

void generateWaves(waves *water);
void initializeWaves(waves *water, int dim);

#endif
