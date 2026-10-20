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
	unsigned int slopeTexture;  // RG32F  dh/dx, dh/dz      (per patch length)
	unsigned int jacobTexture;  // RGB32F dDx/dx, dDz/dz, dDx/dz (per patch length)
	unsigned int detailTexture;
} waves;

void generateWaves(waves *water);
void initializeWaves(waves *water, int dim);

#endif
