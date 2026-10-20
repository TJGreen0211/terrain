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
} PlanetGenParams;

extern PlanetGenParams g_planetGen;

/* Surface look + climate response, uploaded to quad.frag every frame. The
   defaults are Earth-like; every field is free to change for alien worlds
   (palette, temperature, humidity, how strongly the equator is favoured).
   Biome grid: rows cold / temperate / hot, columns dry / medium / wet. */
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
} PlanetStyle;

extern PlanetStyle g_planetStyle;

/* Binds the climate map to texture unit `unit` and uploads g_planetStyle;
   harmless for shaders that don't declare the uniforms. */
void setPlanetSurfaceUniforms(unsigned int shader, int unit);

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
unsigned int generatePlanetHeightmap(unsigned int seed, PlanetGenParams *out,
                                     void (*step)(void));

#endif
