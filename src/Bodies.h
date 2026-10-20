#ifndef BODIES_H
#define BODIES_H

#include "LinearAlg.h"

/* Celestial bodies: physical description (from WorldConfig.h) plus the
   per-frame state derived from the simulation time. Everything is double
   and in metres; the GPU only sees Body_ModelMatrix after the camera is
   subtracted and it is downcast.

   A body's parent must come earlier in the table (parents update first). */
typedef enum BodyId {
	BODY_EARTH,
	BODY_SUN,
	BODY_MOON,
	BODY_COUNT
} BodyId;

typedef struct Body {
	const char *name;
	double radius;          /* m */
	int parent;             /* BodyId, or -1 = the world origin */
	double orbitRadius;     /* m: circle in the world XZ plane around the parent */
	double orbitPeriod;     /* s; 0 = fixed at orbitPhase */
	double orbitPhase;      /* deg at t = 0 */
	double rotationPeriod;  /* s; 0 = no spin */
	double rotationPhase;   /* deg at t = 0 */
	double axialTilt;       /* deg from world +Y */

	/* Per frame (Bodies_Update). */
	dvec3 position;         /* world, m */
	dmat4 rotation;         /* body -> world, rotation only */
} Body;

extern Body g_bodies[BODY_COUNT];

/* Positions and orientations at simulation time t (seconds). */
void Bodies_Update(double t);

/* translate(position - cam) * scale(radius) * rotation, in double: the
   camera-relative model matrix of a unit-radius mesh. Downcast with
   dmat4tomat4 right before upload. */
dmat4 Body_ModelMatrix(const Body *b, dvec3 cam);

#endif
