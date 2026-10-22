#ifndef TERRAINDETAIL_H
#define TERRAINDETAIL_H

#include <glad/glad.h>

/* Terrain detail below the height map's resolution (step 3b): fractal
   gradient noise on land, evaluated in shaders/detail.glsl on an integer
   lattice per octave addressed from the quadtree node, so it is exact at
   any depth. Configured by TERRAIN_DETAIL_* in WorldConfig.h. */

/* Debug panel toggle: 0 = no detail (geometry, bounds and shading). */
extern int g_terrainDetail;
/* Debug panel: detail roughness (TERRAIN_DETAIL_ROUGHNESS) and the baked
   ambient occlusion's strength (0 = off, 1 = full; quad.frag). */
extern float g_detailRoughness;
extern float g_terrainAO;
/* Debug panel: strength of the procedural ground textures (0 = off). */
extern float g_terrainTexStrength;

/* Bakes the procedural ground textures (rock, ground, sand, snow; quad.frag)
   once. GL: main thread, at startup. */
void TerrainDetail_Init(void);

/* Uploads detail.glsl's uniforms (and the terrain look uniforms) for a
   body of `radius` metres (the active planet, g_planetGen), for a pass
   whose quadtree walk targets `trianglePx` (the geometric octave fade). */
void TerrainDetail_SetUniforms(GLuint shader, double radius, double trianglePx);

/* Largest |detail| (body radii) the shaders can add: the quadtree widens
   land bounds by it (QuadTree_Body.detailAmp). 0 when off. */
double TerrainDetail_Amplitude(double radius);

/* Seam a patch edge can open from detail, per unit of patch width: the
   skirt drop's detail term (QuadTree_Body.detailSeam). 0 when off. */
double TerrainDetail_Seam(double radius);

#endif
