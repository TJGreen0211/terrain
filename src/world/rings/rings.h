#ifndef RINGS_H
#define RINGS_H

#include "engine/core/gl.h"
#include "engine/math/linalg.h"

/* Procedural planetary rings.

   Far view: an analytic sheet (ring.frag). A single quad in the ring plane
   only rasterizes the plane (so depth testing against the scene is exact);
   each fragment derives its radius, looks up the baked radial profile and
   shades it as a single-scattering slab (lit / backlit face, planet shadow).

   Close view: instanced rocks + dust points streamed from a hashed grid in
   ring-plane coordinates around the camera (rock.vert / ringdust.vert). The
   grid is anchored in the ring frame, so every cell spawns the same rocks
   each frame with no CPU-side instance data, and the spawn density follows
   the same profile as the sheet, which fades out near the camera.

   The profile is a 1D RGBA32F texture (+ mips, so fine ringlets filter
   instead of aliasing): RGB single-scattering albedo, A normal optical depth.
   Receiver shaders (planet, water, moon/ship, atmosphere) sample it for the
   ring's shadow. One is baked per ring seed and cached, so every ringed
   body's stand-in (planets.c) shades the same profile with the same slab
   model (ringshade.glsl) as the full passes do.

   The full passes (sheet, rocks, dust) run for one ringed body per frame,
   picked by SolarSystem_RingBody: the full-detail planet, a gas giant or a
   stand-in planet alike. */

/* Ring extent in planet radii (sheet, rocks and planets.c's stand-ins). */
#define RING_INNER 1.30f
#define RING_OUTER 2.30f

/* rockVAO/rockVerts: the three rock meshes (initObjectBuffer layout:
   position at 0, normal at 1). */
void Rings_Init(unsigned int seed, const GLuint rockVAO[3], const int rockVerts[3]);
/* The rings the full passes draw this frame: enabled, profile seed (baked
   on first use, then cached) and ring-plane normal (world). Disabled rings
   draw nothing and receivers see no ring shadow. */
void Rings_SetPlanet(int enabled, unsigned int seed, dvec3 normal);
/* Earth's fixed ring plane (the original stylised tilt). */
dvec3 Rings_DefaultNormal(void);

/* Once per receiver program at startup: points ringProfile at its own
   texture unit (otherwise it aliases the program's unit-0 sampler2D, which
   is a draw-time error) and disables the ring until the first frame. */
void Rings_AttachReceiver(GLuint shader);

/* Per frame, after Rings_SetPlanet and before any ring draw or receiver
   bind. Positions are world (double); lightPosition is camera-relative,
   like every shading pass. receivers: the ringed planet is the one the
   receiver programs draw (otherwise they see no ring shadow). */
void Rings_SetFrame(dvec3 planetWorld, dvec3 camWorld, float planetRadius, vec4 lightPosition, int receivers);
/* Ring shadow / attenuation uniforms for a receiver program (quad, water,
   light, atmosphere). Leaves glActiveTexture at unit 0. */
void Rings_BindReceiver(GLuint shader);

/* A stand-in's rings (planet_impostor.frag, program in use): seed's
   profile on the ring unit, ringProfile / ringSunAngle. Leaves
   glActiveTexture at unit 0. */
void Rings_BindStandIn(GLuint shader, unsigned int seed);

/* Opaque rocks: draw with the other opaque geometry. */
void Rings_DrawRocks(mat4 proj, mat4 view);
/* Translucent passes: after the opaque scene, before the atmosphere dome
   (which attenuates its own glow behind the sheet).

   The sheet in two parts round a shell about the planet (radius shell, m;
   the cloud layer's top, which the rings lie outside): behind != 0 draws
   only where the view ray crosses the shell before reaching the sheet, so
   the clouds, drawn after it, cover it as they would anything behind them;
   behind == 0 the rest, after the clouds. shell 0: no shell, all of it in
   the behind == 0 part. */
void Rings_DrawSheet(mat4 proj, mat4 view, float shell, int behind);
void Rings_DrawDust(mat4 proj, mat4 view);

#endif
