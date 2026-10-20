#ifndef TERRAINGEN_H
#define TERRAINGEN_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h>

/* Sea level + ocean depth derived from the baked, eroded height field; both
   shaders must use the identical values or the water discard edge drifts off
   the drawn coastline. Filled by generatePlanetHeightmap(). */
typedef struct {
	float seaLevel;
	float oceanDepth;
	/* RGBA8 climate map baked alongside the height field (same layout, mips):
	   R = moisture (wind-carried rain, rain shadow, anomaly),
	   G = river strength (log drainage area, widened by discharge),
	   B = alluvial deposition from the hydraulic erosion,
	   A = lake depth (depression filling above sea level).
	   Owned by TerrainGen: each bake deletes the previous one. */
	unsigned int climateTex;
	/* RGBA8 ocean map, same layout and mips, from the prevailing winds:
	   R = wind speed / WIND_MAX_MS (latitude cells + storm/calm regions),
	   G, B = wind direction (east, north) * 0.5 + 0.5, the way it blows,
	   A = fetch: how developed the sea is from open water upwind (0 in the
	   lee of land, 1 after a long run of open ocean).
	   Owned by TerrainGen: each bake deletes the previous one. */
	unsigned int oceanTex;
} PlanetGenParams;

extern PlanetGenParams g_planetGen;

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

extern PlanetStyle g_planetStyle;

/* Binds the climate map to texture unit `unit` and uploads g_planetStyle;
   harmless for shaders that don't declare the uniforms. */
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
	float thermalTalus;    /* max stable height step per texel */
	float dropletsPerPx;   /* droplet count = per-px * W * H; dominates bake time */
	float dropletErosion;  /* fraction of free capacity eroded per step */
	float dropletDeposit;  /* fraction of surplus deposited per step */
	float dropletCapacity; /* sediment capacity coefficient */
	float dropletEvap;     /* fraction of water evaporated per step */
	float dropletInertia;  /* 0 = runs straight downhill, 1 = keeps its heading */
} TerrainBakeSettings;

extern TerrainBakeSettings g_terrainBake;

/* Full procedural planet height field bake: GPU shaping (heightbake.frag),
   CPU thermal + hydraulic droplet erosion, percentile normalization, sea
   level statistics, and the 2048x1024 R16F + mip-chain upload. Reentrant:
   call again with a new seed to re-bake (the shader program and working FBO
   are cached; only the returned texture is new and owned by the caller).
   step (may be NULL) is invoked TERRAIN_BAKE_STEPS times through the CPU
   erosion so a loading screen can advance and keep the window responsive. */
#define TERRAIN_THERMAL_STEPS 3    /* evenly spaced through the thermal passes */
#define TERRAIN_HYDRO_STEPS   8    /* evenly spaced through the droplets */
#define TERRAIN_BAKE_STEPS    (TERRAIN_THERMAL_STEPS + TERRAIN_HYDRO_STEPS)
unsigned int generatePlanetHeightmap(const TerrainBakeSettings *settings, PlanetGenParams *out,
                                     void (*step)(void));

#endif
