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
   ring's shadow. */

/* Ring extent in planet radii (sheet, rocks and planets.c's stand-ins). */
#define RING_INNER 1.30f
#define RING_OUTER 2.30f

/* rockVAO/rockVerts: the three rock meshes (initObjectBuffer layout:
   position at 0, normal at 1). */
void Rings_Init(unsigned int seed, const GLuint rockVAO[3], const int rockVerts[3]);
/* Re-bakes the radial profile from a new seed (H key re-roll). */
void Rings_Regenerate(unsigned int seed);

/* The active planet's rings (solar_system.c): enabled, profile seed (re-baked
   only when it changes) and ring-plane normal (world). Disabled rings draw
   nothing and receivers see no ring shadow. */
void Rings_SetPlanet(int enabled, unsigned int seed, dvec3 normal);
/* Earth's fixed ring plane (the original stylised tilt). */
dvec3 Rings_DefaultNormal(void);

/* Once per receiver program at startup: points ringProfile at its own
   texture unit (otherwise it aliases the program's unit-0 sampler2D, which
   is a draw-time error) and disables the ring until the first frame. */
void Rings_AttachReceiver(GLuint shader);

/* Per frame, before any ring draw or receiver bind. Positions are world
   (double); lightPosition is camera-relative, like every shading pass. */
void Rings_SetFrame(dvec3 planetWorld, dvec3 camWorld, float planetRadius, vec4 lightPosition);
/* Ring shadow / attenuation uniforms for a receiver program (quad, water,
   light, atmosphere). Leaves glActiveTexture at unit 0. */
void Rings_BindReceiver(GLuint shader);

/* Opaque rocks: draw with the other opaque geometry. */
void Rings_DrawRocks(mat4 proj, mat4 view);
/* Translucent passes: after the opaque scene, before the atmosphere dome
   (which attenuates its own glow behind the sheet). */
void Rings_DrawSheet(mat4 proj, mat4 view);
void Rings_DrawDust(mat4 proj, mat4 view);

#endif
