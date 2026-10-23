#ifndef CLOUD_VOLUME_H
#define CLOUD_VOLUME_H

#include "engine/core/gl.h"
#include "engine/math/linalg.h"
#include "engine/gfx/frame.h"

/* Volumetric cloud layers around the camera (docs/volumetric-clouds.md
   steps 2-3, docs/gas-giant-interior.md stage 2). Shared by every body
   with clouds; the body's module (the provider) describes them:

   - Layers: shells at altitudes above the body's reference radius, each
     with a density, an albedo tint and shape settings.
   - A weather map: a square patch of the tangent plane centred under the
     camera, CLOUDVOL_MAP_SIZE texels a side, in two RGBA8 textures:
       weather: r = top layer cover, g = middle layer cover, b = height
                (how far the clouds tower to their layer's top),
                a = wind shear across the wind, signed (0.5 = none): the
                clouds lean with it, spread into anvils and shelves
       colour:  rgb = cloud albedo (display), a = walls: full cover,
                built to the layer's top (storm eyewalls)
     and an R8 texture of storm cells: where convection punches up to the
     top (0..1). The provider bakes it with its own full-screen pass between
     CloudVolume_BeginWeatherBake and _EndWeatherBake, and re-bakes when the
     camera has moved far enough (CloudVolume_NeedsBake). _EndWeatherBake
     then smears the cells downwind (by the sign of the shear) into anvil
     plumes: the cells' tops sheared off into long flat sheets near the
     top of a layer, out over clear air (CloudVolumeLayer.plume).
   - The light above the clouds: an exponential atmosphere (optical depth
     of the column at the reference radius, scale height) the Sun shines
     through, its colour and the skylight.

   The march runs at a fraction of the screen resolution, is accumulated
   over frames by reprojection (in the body's frame, so the clouds stay
   put while the camera and the body move), then upsampled over the scene.
   The clouds fog themselves with the same exponential atmosphere (the air
   between the camera and each cloud, integrated along the march), so they
   go over a scene whose own air is already drawn. Layer 0 is the top layer
   (weather r), layer 1 the middle one (weather g).

   Flashes (lightning) are point lights inside the clouds that the provider
   switches on and off; they light the cloud around them and, more
   faintly, the air.

   A shadow map (the clouds' optical depth toward the Sun over a square
   around the camera, CLOUDVOL_SHADOW_SIZE texels a side) shadows the air
   in and under the layers, so sunlight through gaps makes shafts. The
   march uses it; a provider's own air pass can too (CloudVolume_BindShadow
   with cloudshadowmap.glsl).

   Planets (CloudVolumeScene.domeAir, the rocky planets): one layer over
   the whole body, the same clouds from the ground to orbit. Its weather is
   a body-fixed cube map (weatherCube) instead of the local map, and its
   shapes are placed in body coordinates, so nothing re-centres or swaps
   with distance (no weather bake here; the map frame only places the
   shadow map). The air is the O'Neill shell of world/atmosphere instead
   of the exponential air. The march, shadow map and composite are then
   compiled with DOME_AIR: the Sun is reddened and set by sunTransmittance
   as on the ground, the march adds no air, and the composite dims the
   clouds by the air in front of them, adds that air's light and leaves
   their cover in the target's alpha, so the dome drawn after them adds
   the rest of its column times (1 - alpha). The provider sets the shell's
   uniforms (atmosurface.glsl) with airUniforms.

   One provider draws per frame (the camera is in one place): the history
   and the shadow map are dropped by CloudVolume_BeginFrame when no scene
   was drawn last frame. */

#define CLOUDVOL_MAX_LAYERS  2
#define CLOUDVOL_MAP_SIZE    2048
#define CLOUDVOL_MAX_FLASHES 4
#define CLOUDVOL_SHADOW_SIZE 512

typedef struct CloudVolumeLayer {
	float base, top;       /* m above the reference radius */
	float density;         /* extinction (1/m) at full density */
	float tint[3];         /* times the map's albedo (linear) */
	float coverage;        /* times the map's cover */
	float scale;           /* m: the shape noise's period */
	float sweep;           /* east-west stretch of the shapes */
	float shear;           /* shapes lean east with height (m per m) */
	float erosion;         /* detail noise eating the edges, 0..1 */
	float anvil;           /* tops spreading out where the wind shears, 0..1 */
	float shelf;           /* thin flat sheets out from the clouds, 0..1 */
	float ridges;          /* vertical ribs carved into the faces, 0..1 */
	float stretch;         /* the shapes' vertical stretch (1: as wide as tall) */
	float streaks;         /* thin horizontal windswept bands, 0..1 */
	float carpet;          /* cover of low billows filling the layer's lowest part, 0..1 */
	float plume;           /* anvil plumes streaming downwind from the storm cells, 0..1
	                          (one layer only: the first with plume > 0) */
	float broad;           /* pull of the broad regions (~5 shape periods) on the cover:
	                          thick and clear patches the map does not have */
} CloudVolumeLayer;

/* A point light in the clouds (lightning). Its light falls off over
   radius as it diffuses through the cloud. */
typedef struct CloudVolumeFlash {
	dvec3 position;        /* world, m */
	float color[3];        /* linear, x the Sun's light, this frame */
	float radius;          /* m */
} CloudVolumeFlash;

typedef struct CloudVolumeScene {
	int body;              /* the scene's owner; a change resets the history */
	dvec3 center;          /* world, m */
	double radius;         /* reference radius, m */
	dmat4 rotation;        /* body -> world, rotation only */
	/* Weather map frame, body-fixed: its centre on the reference sphere
	   (unit), east and north there, and its width (m). */
	dvec3 mapCenter, mapEast, mapNorth;
	double mapExtent;
	/* The noise's frame, body-fixed, as the map's: the clouds' shapes are
	   placed in it, so it must stay put while the map re-centres (or they
	   jump). Zero centre: the map's frame. */
	dvec3 noiseCenter, noiseEast, noiseNorth;
	int layerCount;
	CloudVolumeLayer layers[CLOUDVOL_MAX_LAYERS];
	/* The light: the Sun through the atmosphere above the clouds. */
	dvec3 sunDir;          /* world, unit, toward the Sun */
	double airColumn;     /* optical depth straight down to the reference radius */
	double airScaleHeight; /* m */
	float terminator[3];   /* the Sun's tint near the terminator */
	float wrap;            /* light wrapping past the terminator */
	float airAlbedo[3];    /* the air's colour (display): in front of the clouds */
	float airAmbient;      /* the air's skylight, x the Sun */
	float airNight;        /* the air's light on the night side */
	float airForward;      /* forward-scattering share of the air's phase, 0..1 */
	float skyColor[3];     /* skylight from above (display) ... */
	float skyGain;         /* ... times this, x the Sun */
	float groundColor[3];  /* light bounced from below (display) ... */
	float groundGain;      /* ... times this, x the Sun */
	float exposure;
	float fade;            /* the clouds' opacity over the scene, 0..1 (the provider's
	                          hand-off: a cross-fade, the clouds themselves unchanged) */
	/* A flat cloud deck under the layers (deckOn): opaque, drawn to the
	   horizon as a lumpy surface at deckAlt with a mist rising off it, so
	   the layers' bottoms sink into it. */
	int deckOn;
	float deckAlt;         /* m above the reference radius */
	float deckColor[3];    /* albedo (display) */
	float deckScale;       /* m: its lumps */
	float deckMist;        /* m: the mist's scale height above and below it */
	float deckDensity;     /* the mist's extinction at the deck (1/m) */
	double reach;          /* m: how far the march reaches (0: CloudVolume_Reach) */
	int flashCount;
	CloudVolumeFlash flashes[CLOUDVOL_MAX_FLASHES];
	/* A planet (above): the air*, terminator and wrap settings are then
	   unused, skyColor / groundColor too (the sky and ground light come
	   from skyLight, times skyGain / groundGain), the layer's albedo is albedo, and mapCenter / East / North only place the
	   shadow map (no extent; reach 0: no limit). */
	int domeAir;
	GLuint weatherCube;    /* body-fixed: r cover, g how high the clouds build (0..1 of
	                          the layer), b optical thickness (0..1), a wispiness (1 away
	                          from weather systems: eroded more); mipmapped */
	int weatherCubeSize;   /* texels a face side */
	double weatherTexelM;  /* m: a texel at the reference radius */
	float albedo[3];       /* linear */
	float sunColor[3];     /* the Sun's light on the clouds (linear, before the air) */
	float sunAngle;        /* its angular radius (rad): it sets behind the limb */
	GLuint skyLight;       /* the sky's (row 0) and the ground's (row 1) light on the
	                          clouds by the Sun's cosine at the point, -0.35 .. 1
	                          (linear, sunColor's units: clouds.c, cloudsky.frag) */
	/* Sets atmosurface.glsl's uniforms on the program in use (the march's
	   and the composite's), shell centred on the reference sphere's. */
	void (*airUniforms)(GLuint program);
} CloudVolumeScene;

/* Quality and look shared by every body (debug panel). */
typedef struct CloudVolumeParams {
	int enabled;
	int resDivisor;        /* march at 1/n of the screen per axis */
	int temporalBlock;     /* 1, or 2: each frame marches one pixel of every 2x2 block (cycling),
	                          the others come from the accumulated history */
	int viewSteps;         /* max samples per view ray */
	int lightSteps;        /* samples toward the Sun */
	float maxDistance;     /* m: the march ends here (fading out before) */
	float history;         /* weight of the accumulated frames, 0..0.98 */
	float detailScale;     /* detail noise period / shape noise period */
	float multiScatter;    /* strength of the multiple-scattering octaves */
	float powder;          /* darkening of thin cloud facing the Sun */
	float ambient;         /* skylight strength */
	float shadowExtent;    /* m: the shadow map's width (0: no shadow map) */
	int debugView;         /* 0 off; heat map of each pixel's 1 march steps, 2 density calls */
} CloudVolumeParams;
CloudVolumeParams *CloudVolume_Params(void);

/* --cloud-debug <n>: start with debugView n and log the march's mean
   steps and density calls per pixel (every DEBUG_STATS_EVERY frames) on
   stderr. --cloud-block <n>: start with temporalBlock n. --cloud-volume
   <0|1>: start with the volume off or on. */
void CloudVolume_ParseArgs(int argc, char *argv[]);

/* How far the march reaches (m): the Max distance setting, or farther
   when the camera is high above the layers (aboveLayers: its height over
   their lowest base, m), but within the weather map. The clouds fade out
   by their opacity from 35% of it; a provider fading anything into them
   uses the same.
   A scene's own reach replaces it (still within the map). The march also
   ends where the air has hidden everything. */
double CloudVolume_Reach(double aboveLayers, double mapExtent);

/* Programs, noise volumes and the weather map (GL up). */
void CloudVolume_Init(void);
/* Once per frame, before any provider: drops the shadow map, and the
   history if no scene was drawn last frame. */
void CloudVolume_BeginFrame(void);
/* The main pass's depth (render.c, on a resize). */
void CloudVolume_SetSceneDepth(GLuint depthTex, int samples, float nearPlane);

/* Whether the weather map must be re-baked for this frame: a new body, or
   the camera's ground point more than an eighth of the map from its
   centre. cameraBody: the camera in body-fixed axes (m). */
int CloudVolume_NeedsBake(int body, dvec3 cameraBody, double radius, double extent);
/* Binds the weather map as the render target (all three textures,
   viewport set). The provider draws one full-screen pass writing
   locations 0 (weather), 1 (colour) and 2 (storm cells). End builds the
   plumes (plumeLength m long, over a map mapExtent m wide), restores the
   caller's framebuffer and viewport and records which body / centre the
   map is for. */
void CloudVolume_BeginWeatherBake(void);
void CloudVolume_EndWeatherBake(int body, dvec3 mapCenter, double mapExtent, double plumeLength);
/* Drops the map (it no longer matches any provider). */
void CloudVolume_Invalidate(void);

/* Rebuilds the shadow map for this frame's scene (before anything reads
   it: the provider's air pass, then CloudVolume_Draw). Nothing when
   disabled or s->fade is 0 (the map stays off this frame). */
void CloudVolume_UpdateShadow(const FrameContext *f, const CloudVolumeScene *s);
/* Sets cloudshadowmap.glsl's uniforms on the program in use, with the map
   on texture unit `unit` (csOn = 0 when there is no map this frame). */
void CloudVolume_BindShadow(GLuint program, int unit, const FrameContext *f);

/* Marches s and blends it over the bound target (the main pass, after the
   scene's own air). Nothing when disabled or s->fade is 0. Times its
   passes as PROF_CLOUD_MARCH, _ACCUM and _COMPOSITE. */
void CloudVolume_Draw(const FrameContext *f, const CloudVolumeScene *s);

struct nk_context;
/* The "Volume" group of the Clouds tab. */
void CloudVolume_DebugPanel(struct nk_context *ctx);

#endif
