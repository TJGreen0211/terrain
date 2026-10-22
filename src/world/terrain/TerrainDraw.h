#ifndef TERRAINDRAW_H
#define TERRAINDRAW_H

#include "engine/core/gl.h"
#include "engine/gfx/Frame.h"
#include "world/bodies/Bodies.h"
#include "world/terrain/QuadTree.h"

/* Drawing the active planet's surface: the quad.vert/quad.frag programs,
   the relief bounds the quadtree walks cull and refine with, the uniform
   recipe every surface program shares (land, seafloor and the water shell,
   WaterDraw.c), and the planet's shadow casters. */

/* Baked planet height map (R32F cube map + mips, TerrainGen.c), sampled by
   quad.frag/water.frag instead of evaluating simplex noise per pixel. The
   active planet's (SolarSystem.c swaps it). */
GLuint TerrainDraw_HeightTex(void);
void TerrainDraw_SetHeightTex(GLuint tex);

/* The surface programs: colour, and the depth-only prepass variant. */
void TerrainDraw_Init(void);
GLuint TerrainDraw_SurfaceProgram(void);   /* colour (ring-shadow receiver) */

/* The planet as the quadtree sees it: camera-relative model, `relief` for
   the surface being drawn, the deepest seafloor as the horizon occluder
   (water is see-through). TerrainDraw_PlanetBody: land + seafloor, with
   the detail noise's amplitude and seam. */
QuadTree_Body TerrainDraw_Body(const Body *planet, dvec3 cam, QuadTree_ReliefFn relief);
QuadTree_Body TerrainDraw_PlanetBody(const Body *planet, dvec3 cam);

/* 1 if every height a fragment shader can sample over the cap is above
   sea level, i.e. no pixel there is sea (water.frag discards them all). */
int TerrainDraw_FootprintAllLand(const double dir[3], double capAngle, double maxDist);

/* View distances the terrain can be seen over (for the shadow cascades):
   nearest = the camera's height above the highest terrain within its
   horizon; farthest = the horizon plus as far as its peaks can peek over. */
void TerrainDraw_ViewRange(const Body *planet, dvec3 cam, double *nearest, double *farthest);

/* Radius (m) of Earth's highest terrain within capAngle of a world
   direction, detail noise included (Bench.c's 'poff ground'). */
double Earth_GroundRadius(dvec3 worldDir, double capAngle);

/* Uniform + texture-unit recipe for the quadtree body passes (binds the
   program). `m` is the body's camera-relative model; its translation column
   is the body centre. The wave maps are 2D arrays, one layer per cascade
   (0 for the planet); tex2 goes to noiseTexture/detailNormal. */
void TerrainDraw_SetBodyUniforms(GLuint shader, float lodRadius, mat4 m, const FrameContext *f,
                                 GLuint tex0, GLuint tex2, GLuint slopeTex, GLuint dispTex,
                                 GLuint jacobTex, GLuint skyTex);

/* The planet as a shadow caster into the bound shadow target (lightSpace:
   the global map's or a cascade's), through the walk with the main pass's
   displacement and skirts (depth.vert), at trianglePx and no finer than
   minSpacing; cullNear as QuadTree_DrawShadowCasters. */
void TerrainDraw_ShadowCasters(const QuadTree_Body *body, mat4 lightSpace, double minSpacing,
                               double trianglePx, int cullNear, const FrameContext *f);

/* Land + seafloor: depth prepass, then colour over it. atmoInner: the
   atmosphere shell's inner radius; bodyId: the planet (its clouds). */
void TerrainDraw_Planet(const QuadTree_Body *body, const FrameContext *f, float atmoInner,
                        int bodyId, GLuint noiseTex);

#endif
