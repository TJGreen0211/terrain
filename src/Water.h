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

typedef struct waves {
	complexType *dx;
	complexType *dy;
	complexType *dz;
	complexType *tildeh0k;
	complexType *conjTildeh0MK;
	int dimension;
	unsigned int dxTexture;
	unsigned int dyTexture;
	unsigned int dzTexture;
} waves;

void generateWaves(waves *water);
void initializeWaves(waves *water, int dim);

#endif
