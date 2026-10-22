#ifndef TERRAINGEN_H
#define TERRAINGEN_H
#include "engine/core/gl.h"
#include <math.h>
#include "world/WorldConfig.h"

/* CPU min/max pyramid of a baked height field (TerrainGen.c). */
typedef struct TerrainPyramid TerrainPyramid;

/* One baked planet: sea level + ocean depth derived from the baked, eroded
   height field (both shaders must use the identical values or the water
   discard edge drifts off the drawn coastline), its climate and ocean maps
   and its height-bounds pyramid. Filled by generatePlanetHeightmap(); owned
   by the caller (TerrainGen_FreePlanet). TerrainGen_GenParams() is the active planet:
   the one the body shaders and the bounds queries below use. */
typedef struct {
	float seaLevel;
	float oceanDepth;
	/* RGBA8 climate map baked alongside the height field (the same cube-map
	   layout, mips):
	   R = moisture (wind-carried rain, rain shadow, anomaly),
	   G = river strength (log drainage area, widened by discharge),
	   B = alluvial deposition from the hydraulic erosion,
	   A = lake depth (depression filling above sea level).
	   Freed by TerrainGen_FreePlanet. */
	unsigned int climateTex;
	/* RGBA8 ocean map (cube map), same layout and mips, from the prevailing
	   winds:
	   R = wind speed / WIND_MAX_MS (latitude cells + storm/calm regions),
	   G, B = wind direction (east, north) * 0.5 + 0.5, the way it blows,
	   A = fetch: how developed the sea is from open water upwind (0 in the
	   lee of land, 1 after a long run of open ocean).
	   Freed by TerrainGen_FreePlanet. */
	unsigned int oceanTex;
	/* RGBA16F surface map (cube map, mips): RGB = the height field's slope,
	   a body-local tangent vector in raw height units per radian; A =
	   ambient occlusion (cosine-weighted open sky, 0..1). Freed by
	   TerrainGen_FreePlanet. */
	unsigned int surfaceTex;
	/* RMS raw-height step between neighbouring land texels: the map's own
	   roughness at its finest scale (the detail noise is sized from it). */
	float landStepRms;
	/* Height bounds for the quadtree (TerrainGen_HeightRange). */
	TerrainPyramid *pyramid;
} PlanetGenParams;

/* The active planet's (SolarSystem.c swaps them, like the style and the
   bake settings below). */
PlanetGenParams *TerrainGen_GenParams(void);

/* Surface look + climate response, uploaded to quad.frag every frame. The
   defaults are Earth-like; every field is free to change for alien worlds
   (palette, temperature, humidity, how strongly the equator is favoured).
   Biome grid: rows cold / temperate / hot, columns dry / medium / wet.
   All colours are sRGB; they are converted to linear albedo on upload. */
typedef struct {
	float biome[3][3][3];
	float rock[3], snow[3], sand[3], salt[3], water[3];
	float tempOffset;      /* + warms the whole planet, - cools it */
	float lapseRate;       /* temperature drop from sea level to the top */
	float snowTemp;        /* temperature below which snow/ice settles */
	float humidity;        /* global moisture multiplier */
	float equatorLush;     /* 0 = no equatorial rain belt, 1 = Earth, >1 wetter */
	float hadleyStrength;  /* 0 = ignore latitude rain bands, 1 = full */
	float riverThreshold;  /* river map value where rivers become visible */
	float rockSlope;       /* slope (rise/run) above which bare rock shows */
	/* Geometric relief as a fraction of the planet radius (quad.vert,
	   depth.vert, water.frag). Land rises up to landRelief above sea level;
	   the seafloor sinks to oceanRelief below it along a squared
	   (continental shelf) profile. landRelief must stay under
	   QT_CULL_RELIEF (QuadTree.c) and inside the atmosphere shell. */
	float landRelief;
	float oceanRelief;
} PlanetStyle;

/* Live: uploaded every frame (debug panel). */
PlanetStyle *TerrainGen_Style(void);

/* Binds the climate map to texture unit `unit` (and the surface map to
   TERRAIN_SURFACE_UNIT) and uploads TerrainGen_Style(); harmless for shaders
   that don't declare the uniforms. */
#define TERRAIN_SURFACE_UNIT 16
void setPlanetSurfaceUniforms(unsigned int shader, int unit);

/* Binds the ocean map to texture unit `unit` for water.vert/.frag. */
void setOceanUniforms(unsigned int shader, int unit);

/* Bake inputs: everything generatePlanetHeightmap() reads besides fixed
   pipeline constants. Edited from the debug panel; changes apply on the
   next bake, not live. */
#define TERRAIN_PLATE_MAX 32 /* MAX_PLATES in heightbake.frag */
typedef struct {
	unsigned int seed;
	/* Tectonic plates (CPU) */
	int plateCount;        /* 2..TERRAIN_PLATE_MAX: more = smaller continents, more ranges */
	float continentArea;   /* share of the sphere on continental plates (~ land fraction) */
	/* Shaping (heightbake.frag uniforms), raw shaper units, sea level = 0 */
	float coastFreq;       /* coastline roughness frequency */
	float coastAmt;        /* coastline roughness amplitude */
	float plainsH;         /* continental interior base height */
	float hillAmt;         /* hill country relief */
	float plateauH;        /* raised flat plateaus */
	float collideH;        /* suture range height (continent/continent) */
	float andesH;          /* coastal range height (continent over ocean) */
	float oldRangeH;       /* old eroded intraplate ranges */
	float oceanBase;       /* abyssal plain depth (negative) */
	float ridgeH;          /* mid-ocean ridge rise */
	float hotspotLo;       /* volcanic island threshold: higher = rarer */
	float fragLo;          /* microcontinent threshold: higher = fewer */
	float detailAmt;       /* high-frequency rock texture */
	/* Erosion (CPU), in pre-normalized [0,1] height units */
	int thermalPasses;     /* weathering iterations */
	float thermalTalus;    /* max stable height step per reference texel of distance */
	float dropletsPerPx;   /* droplet count = per-px * 2048 * 1024 (the reference
	                          equirect grid, TerrainGen.c), whatever the map
	                          size; dominates bake time */
	float dropletErosion;  /* fraction of free capacity eroded per step */
	float dropletDeposit;  /* fraction of surplus deposited per step */
	float dropletCapacity; /* sediment capacity coefficient */
	float dropletEvap;     /* fraction of water evaporated per step */
	float dropletInertia;  /* 0 = runs straight downhill, 1 = keeps its heading */
} TerrainBakeSettings;

/* Bake inputs: edits apply on the next bake (debug panel). */
TerrainBakeSettings *TerrainGen_BakeSettings(void);

/* Full procedural planet height field bake: GPU shaping (heightbake.frag),
   CPU thermal + hydraulic droplet erosion, percentile normalization, sea
   level statistics, and the R32F cube map (TERRAIN_MAP_SIZE per face) +
   mip-chain upload. Reentrant:
   call again with a new seed to re-bake (the shader program and working FBO
   are cached; only the returned texture is new and owned by the caller).
   style: the baked planet's look (its humidity, rain belts and relief
   shape the rivers and occlusion), not necessarily the active planet's.
   step (may be NULL) is invoked TERRAIN_BAKE_STEPS times through the CPU
   erosion so a loading screen can advance and keep the window responsive. */
#define TERRAIN_THERMAL_STEPS 3    /* evenly spaced through the thermal passes */
#define TERRAIN_HYDRO_STEPS   8    /* evenly spaced through the droplets */
#define TERRAIN_BAKE_STEPS    (TERRAIN_THERMAL_STEPS + TERRAIN_HYDRO_STEPS)
unsigned int generatePlanetHeightmap(const TerrainBakeSettings *settings, const PlanetStyle *style,
                                     PlanetGenParams *out, void (*step)(void));

/* Background bake: the same bake without the stall. The GPU shaping pass
   is drawn in strips over successive polls and read back asynchronously;
   the CPU part (erosion, climate maps, bounds pyramid: nearly all of the
   time) runs on a worker thread. Poll once a frame from the GL thread; it
   returns 1 when the job is ready, then Finish uploads the maps into out
   and returns the height texture, like generatePlanetHeightmap but
   without making the planet active. Cancel drops a job. One bake at a
   time: a start or a blocking bake first completes a running one. */
typedef struct TerrainBakeJob TerrainBakeJob;
TerrainBakeJob *TerrainGen_BakeStart(const TerrainBakeSettings *settings, const PlanetStyle *style);
int TerrainGen_BakePoll(TerrainBakeJob *job);
float TerrainGen_BakeProgress(const TerrainBakeJob *job);   /* 0..1, rough */
unsigned int TerrainGen_BakeFinish(TerrainBakeJob *job, PlanetGenParams *out);
void TerrainGen_BakeCancel(TerrainBakeJob *job);

/* Frees a baked planet's climate / ocean maps and bounds pyramid (not its
   height texture, which generatePlanetHeightmap returns separately). */
void TerrainGen_FreePlanet(PlanetGenParams *p);

/* Height bounds of the active planet (TerrainGen_GenParams(); raw field units, like
   seaLevel), for the quadtree's culling and split distances. HeightRange covers every value
   the body shaders can sample over a spherical cap (dir: body-local unit
   centre, capAngle in radians) at mip `lod` or finer, bilinear and
   trilinear filtering included. lod is sized against a face-centre texel
   (TerrainGen_MapTexelRad) and not clamped: the shaders' face-corner bias
   is added inside, before the clamp to mip 0, as the shaders do. Both
   return 0 (range 0..0) before the first bake. */
/* The baked maps' geometry (WorldConfig.h TERRAIN_MAP_SIZE): a face-centre
   texel's angle on the sphere (radians, mip 0) and the coarsest mip. */
static inline double TerrainGen_MapTexelRad(void) { return 2.0 / TERRAIN_MAP_SIZE; }
static inline double TerrainGen_MapMaxLod(void) { return log2((double)TERRAIN_MAP_SIZE); }
int TerrainGen_HeightRange(const double dir[3], double capAngle, double lod, float *hmin, float *hmax);
int TerrainGen_GlobalRange(float *hmin, float *hmax);
/* GlobalRange of any baked planet, active or not. */
int TerrainGen_PlanetRange(const PlanetGenParams *planet, float *hmin, float *hmax);

#endif
