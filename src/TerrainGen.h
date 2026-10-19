#ifndef TERRAINGEN_H
#define TERRAINGEN_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h>

/* Sea level + ocean depth derived from the baked, eroded height field; both
   shaders must use the identical values or the water discard edge drifts off
   the drawn coastline. Filled by generatePlanetHeightmap(). */
typedef struct {
	float seaLevel;
	float oceanDepth;
} PlanetGenParams;

extern PlanetGenParams g_planetGen;

/* Full procedural planet height field bake: GPU shaping (heightbake.frag),
   CPU thermal + hydraulic droplet erosion, percentile normalization, sea
   level statistics, and the 2048x1024 R16F + mip-chain upload. Reentrant:
   call again with a new seed to re-bake (the shader program and working FBO
   are cached; only the returned texture is new and owned by the caller). */
unsigned int generatePlanetHeightmap(unsigned int seed, PlanetGenParams *out);

#endif
