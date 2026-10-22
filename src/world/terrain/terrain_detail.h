#ifndef TERRAIN_DETAIL_H
#define TERRAIN_DETAIL_H

#include "engine/core/gl.h"

/* Terrain detail below the height map's resolution (step 3b): fractal
   gradient noise on land, evaluated in shaders/detail.glsl on an integer
   lattice per octave addressed from the quadtree node, so it is exact at
   any depth. Configured by TERRAIN_DETAIL_* in WorldConfig.h. */

/* Live, from the debug panel. */
typedef struct TerrainDetailParams {
	int enabled;         /* 0 = no detail (geometry, bounds and shading) */
	float roughness;     /* detail roughness (starts at TERRAIN_DETAIL_ROUGHNESS) */
	float ao;            /* baked ambient occlusion strength (0 = off, 1 = full; quad.frag) */
	float texStrength;   /* procedural ground textures (0 = off) */
} TerrainDetailParams;
TerrainDetailParams *TerrainDetail_Params(void);

/* Bakes the procedural ground textures (rock, ground, sand, snow; quad.frag)
   once. GL: main thread, at startup. */
void TerrainDetail_Init(void);

/* Uploads detail.glsl's uniforms (and the terrain look uniforms) for a
   body of `radius` metres (the active planet, TerrainGen_GenParams()), for a pass
   whose quadtree walk targets `trianglePx` (the geometric octave fade). */
void TerrainDetail_SetUniforms(GLuint shader, double radius, double trianglePx);

/* Largest |detail| (body radii) the shaders can add: the quadtree widens
   land bounds by it (QuadTree_Body.detailAmp). 0 when off. */
double TerrainDetail_Amplitude(double radius);

/* Seam a patch edge can open from detail, per unit of patch width: the
   skirt drop's detail term (QuadTree_Body.detailSeam). 0 when off. */
double TerrainDetail_Seam(double radius);

#endif
