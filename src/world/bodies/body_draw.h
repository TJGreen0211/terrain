#ifndef BODY_DRAW_H
#define BODY_DRAW_H

#include "engine/core/gl.h"
#include "engine/gfx/frame.h"
#include "world/bodies/bodies.h"

/* The lit, textured bodies drawn in full: the Moon (as an LOD quadtree
   sphere) and the sun marker sphere. One program (light.vert/light.frag)
   for both; it is also a ring-shadow receiver (BodyDraw_Program). */
void BodyDraw_Init(void);
GLuint BodyDraw_Program(void);

/* The sun marker sphere at the sun body (whose world position is the
   light's): mesh vao with `vertices` vertices, textured with `texture`. */
void BodyDraw_SunMarker(GLuint vao, int vertices, GLuint texture, const Body *sun, const FrameContext *f);

/* The Moon as an LOD quadtree sphere (same face-grid geometry as the
   planet), textured with `texture` (equirectangular), receiving the
   planet's shadow. */
void BodyDraw_Moon(GLuint texture, const Body *moon, const FrameContext *f);

#endif
