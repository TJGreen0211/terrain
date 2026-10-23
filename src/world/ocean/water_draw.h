#ifndef WATER_DRAW_H
#define WATER_DRAW_H

#include "engine/core/gl.h"
#include "engine/gfx/frame.h"
#include "world/bodies/bodies.h"
#include "world/ocean/water.h"

/* Drawing the water shell (water.vert/water.frag): the FFT-displaced
   sea-level shell over the active planet, through its own quadtree walk,
   discarding over land in-shader. Surface uniforms come from
   TerrainDraw_SetBodyUniforms, the same recipe as the land. */
void WaterDraw_Init(void);
GLuint WaterDraw_Program(void);   /* ring-shadow receiver */

/* water.vert's geometric wave scale on a body of this radius (m): the FFT
   field's rms (body radii) mapped to OCEAN_SWELL_RMS_M of real swell. */
double Water_GeomScale(double radius);
/* How far (body radii) the displaced shell can stray from sea level: the
   water walk's culling bound and the seafloor clearance quad.frag keeps. */
double Water_Excursion(double radius);
/* The shell's flat triangles cut inside the sphere between their vertices:
   at camera distance d (m) by up to (d * this)^2 body radii. Its walk is
   coarser than the land's (QT_WATER_TRIANGLE_PX), so quad.frag keeps the
   seafloor that much further back too. */
double Water_SagK(double radius);

/* The shell over `planet`, alpha-blended (premultiplied) over the land.
   skyTex: the cube map water.frag reflects; atmoInner: the atmosphere
   shell's inner radius; bodyId: the planet (its clouds). */
void WaterDraw_Draw(const waves *ocean, const Body *planet, const FrameContext *f,
                    GLuint skyTex, float atmoInner, int bodyId);

#endif
