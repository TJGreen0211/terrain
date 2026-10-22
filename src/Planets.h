#ifndef PLANETS_H
#define PLANETS_H

#include <glad/glad.h>
#include "engine/math/LinearAlg.h"
#include "Bodies.h"

/* Stand-ins (shaders/planet_impostor.*) for every body not drawn in full
   this frame: gas giants always, terrain planets while they are inactive or
   small, the Moon while small. Each is a camera-facing quad around its
   silhouette (and rings / atmosphere), ray-traced as an exact sphere,
   Lambert-lit with body-fixed mottling, an atmosphere glow at the limb and
   a ring sheet that shadows the planet and is shadowed by it; it writes
   depth. Below ~1.5 px a body becomes a soft additive dot that keeps the
   disc's total light (with a floor so planets read like bright stars). */

void Planets_Init(void);

/* Radius of the body on screen, in pixels (at the view centre); 0 when the
   camera is inside it. cam: world camera position. */
double Planets_ScreenRadiusPx(const Body *b, dvec3 cam);

/* 1 when a body is big enough on screen for its full-detail passes. */
int Planets_FullDetail(const Body *b, dvec3 cam);

/* Draws the stand-ins into the bound (multisampled, reversed-Z) scene
   target: discs first, then additive dots (depth-tested, no depth write).
   proj / view: the main camera's. fullBody: the terrain planet drawn in
   full this frame (-1 = none); moonFull: the Moon is drawn in full. */
void Planets_Draw(mat4 proj, mat4 view, dvec3 cam, int fullBody, int moonFull);

#endif
