#ifndef CLOUDS_H
#define CLOUDS_H

#include "engine/core/gl.h"
#include "engine/math/LinearAlg.h"

/* Cloud layers (docs/volumetric-clouds.md). Every planet with air can have
   one; its settings live in PlanetInfo.clouds, and the active planet's in
   Clouds_Params() (saved and loaded with the other planet globals, edited live from
   the debug panel). Step 1 draws the layer as a slab at its middle altitude:

   - Weather maps, per baked planet: coverage and column thickness in the
     climate map's equirect layout, built on the GPU from the planet's
     moisture and wind maps and moved along the wind with a two-phase flow
     map. Rebuilt every CLOUD_MAP_STEP_S of simulation time; the two newest
     are crossfaded.
   - The slab over the full-detail planet, with detail noise below the map.
   - Shadows on the planet and water passes (Clouds_BindReceiver).
   - The clouds on the stand-ins (Clouds_BindStandIn). */

typedef enum CloudStyle {
	CLOUD_STYLE_NONE,
	CLOUD_STYLE_CLIMATE,    /* coverage follows moisture and latitude belts */
	CLOUD_STYLE_PATCHY,
	CLOUD_STYLE_BANDED,     /* latitude bands */
	CLOUD_STYLE_OVERCAST,   /* a near-unbroken deck (Venus) */
	CLOUD_STYLE_COUNT
} CloudStyle;

typedef struct cloudParams {
	int style;              /* CloudStyle */
	float coverage;         /* target sky fraction, 0..1 */
	float climateWeight;    /* climate style: pull of moisture + latitude belts */
	float altitude;         /* m above sea level, layer middle */
	float thickness;        /* m, layer depth */
	float opacity;          /* optical depth of a full column */
	float weatherScale;     /* largest weather system's wavelength, planet radii */
	float sharpness;        /* 0 = soft edges, 1 = hard */
	float detail;           /* erosion by the small-scale noise, 0..1 */
	float bands;            /* banded style: bands from pole to pole */
	float color[3];         /* albedo (linear) */
	float flowPeriod;       /* simulation seconds per flow cycle; 0 = frozen */
	unsigned int seed;
} cloudParams;

/* The active planet's live cloud layer (SolarSystem.c swaps it; debug
   panel). The panel also sets the slab's detail octaves (starts at
   CLOUD_SLAB_OCTAVES) and the master switch (CLOUDS_ENABLED_DEFAULT; off
   drops every planet's weather maps, so nothing is built, drawn or
   shadowed). */
cloudParams *Clouds_Params(void);
cloudParams cloudDefaults(void);   /* Earth's (WorldConfig.h) */
const char *Clouds_StyleName(int style);

/* Programs and the map target; after Rings_Init. */
void Clouds_Init(void);

/* Per frame, after SolarSystem_Update: builds the weather maps every baked
   planet with clouds needs at simTime (at most CLOUD_MAP_BUILDS_PER_FRAME,
   the active planet first). */
void Clouds_Update(double simTime);

/* Forgets body's maps (its bake was freed or re-done). */
void Clouds_Drop(int body);

/* Cloud shadows for a planet / water program (bound by the caller): maps on
   texture units 14 and 15. body: the active planet. */
void Clouds_BindReceiver(GLuint shader, int body);

/* The slab over body (the active, full-detail planet) into the bound scene
   target, after the water and before the rings and the atmosphere dome. It
   leaves its alpha in the target's alpha channel (which must be 0 before),
   for the dome to scale its in-scatter by. proj / view: the main camera's. */
void Clouds_Draw(mat4 proj, mat4 view, dvec3 cam, int body);

/* Clouds on body's stand-in (planet_impostor.frag, bound): maps on texture
   units 0 and 1; switched off when body has none. */
void Clouds_BindStandIn(GLuint shader, int body);

struct nk_context;
/* The Clouds tab of the debug panel (Clouds.c). */
void Clouds_DebugPanel(struct nk_context *ctx);

#endif
