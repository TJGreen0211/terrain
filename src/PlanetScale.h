#ifndef PLANETSCALE_H
#define PLANETSCALE_H

/* Physical radius the planet represents. The scene draws it at a few
   hundred world units; everything with real-world units (wave tiles in
   metres, wind fetch in km, pixel footprints) converts through this one
   value. */
#define PLANET_RADIUS_M 6371000.0

#endif
