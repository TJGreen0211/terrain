#ifndef SOLARSYSTEM_H
#define SOLARSYSTEM_H

#include "engine/core/gl.h"
#include "world/bodies/Bodies.h"
#include "world/terrain/TerrainGen.h"
#include "world/atmosphere/Atmosphere.h"
#include "world/clouds/Clouds.h"

/* The procedural solar system: every planet but Earth (and its Moon) is
   generated from the system seed (separate from Earth's terrain seed),
   and every terrain planet (Earth, rocky planets, dwarf planets) can be
   drawn in full detail.

   Only one terrain planet is drawn in full at a time: the active one. Its
   state lives in the globals the planet passes have always used
   (TerrainGen_GenParams(), TerrainGen_Style(), Atmosphere_Params(), Clouds_Params(), TerrainGen_BakeSettings(),
   TerrainDraw_HeightTex(), the ring state), so the debug panel edits whichever
   planet is active. Activating another planet saves those into its
   PlanetInfo and loads the new one. A planet's terrain is baked the first
   time it is approached, in the background (TerrainGen_BakeStart), and
   cached until the system is re-generated or the app restarts. */

typedef enum PlanetType {
	PLANET_NONE,         /* not a planet (Sun, Moon) */
	PLANET_EARTHLIKE,    /* Earth: fixed, as hand-built */
	PLANET_ROCKY,
	PLANET_DWARF,
	PLANET_GAS           /* stand-in only (Planets.c) */
} PlanetType;

typedef struct PlanetInfo {
	PlanetType type;
	int hasAtmosphere;
	int hasWater;            /* generated; only Earth's is drawn (OTHER_PLANET_WATER) */
	int hasRings;
	unsigned int ringSeed;
	float ringColor[3];      /* stand-in ring tint */
	TerrainBakeSettings bake;
	PlanetStyle style;
	atmosphereParams atmo;   /* shellHeight 0 = no atmosphere */
	cloudParams clouds;      /* style none = no cloud layer */
	/* Baked terrain (rocky / dwarf / Earth). */
	int baked;
	GLuint heightTex;
	PlanetGenParams gen;
} PlanetInfo;

/* Body id (BodyId). */
PlanetInfo *SolarSystem_Info(int id);

typedef struct SolarSystemParams {
	unsigned int seed;   /* the system seed (SolarSystem_Generate); starts at SYSTEM_SEED_DEFAULT */
} SolarSystemParams;
SolarSystemParams *SolarSystem_Params(void);

/* After Earth's start-up bake: records Earth (the current globals) as its
   planet and generates the rest of the system from the system seed. */
void SolarSystem_Init(void);

/* Re-generates every planet but Earth from seed: orbits, sizes, kinds and
   looks. Drops their cached bakes; Earth becomes active if it was not. */
void SolarSystem_Generate(unsigned int seed);

/* The body whose state is in the planet globals. */
int SolarSystem_ActiveBody(void);

/* The terrain planet that should be active for this camera: the largest
   on screen once it is a few pixels across, else the current one. */
int SolarSystem_PickActive(dvec3 cam);

/* Makes body the active planet, baking it first if needed (step: the bake's
   progress callback, may be NULL). Blocks for the bake. */
void SolarSystem_Activate(int body, void (*step)(void));

/* Per frame, after the camera moves: takes a finished background bake,
   switches to PickActive's planet once it is baked (starting its bake in
   the background otherwise; it stays a stand-in meanwhile) and pre-bakes
   the focused planet when the bake slot is free. blocking: switch and bake
   at once instead (bench runs: repeatable frames). */
void SolarSystem_Update(dvec3 cam, int focusBody, int blocking);

/* The planet baking in the background (-1 = none) and its rough progress
   (0..1, may be NULL). */
int SolarSystem_BakingBody(float *progress);

/* Re-bakes the active planet with the current TerrainGen_BakeSettings() (H key / debug
   panel); Earth's rings follow its seed as before. */
void SolarSystem_RebakeActive(void (*step)(void));

/* "Rocky", "Gas giant", ... for the debug panel. */
const char *SolarSystem_TypeName(PlanetType t);

struct nk_context;
/* The Solar system tab of the debug panel (SolarSystem.c). */
void SolarSystem_DebugPanel(struct nk_context *ctx);
/* 1 once after the panel changed the system seed (the app then calls
   SolarSystem_Generate). */
int SolarSystem_ConsumeNewSystem(void);

#endif
