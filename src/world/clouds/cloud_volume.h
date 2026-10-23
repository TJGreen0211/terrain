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
                (how far the clouds tower to their layer's top), a = unused
       colour:  rgb = cloud albedo (display), a = unused
     The provider bakes it with its own full-screen pass between
     CloudVolume_BeginWeatherBake and _EndWeatherBake, and re-bakes when the
     camera has moved far enough (CloudVolume_NeedsBake).
   - The light above the clouds: an exponential atmosphere (optical depth
     of the column at the reference radius, scale height) the Sun shines
     through, its colour and the skylight.

   The march runs at a fraction of the screen resolution, is accumulated
   over frames by reprojection (in the body's frame, so the clouds stay
   put while the camera and the body move), then upsampled over the scene.
   The clouds fog themselves with the same exponential atmosphere (the air
   between the camera and each cloud), so they go over a scene whose own
   air is already drawn. Layer 0 is the top layer (weather r), layer 1 the
   middle one (weather g). */

#define CLOUDVOL_MAX_LAYERS 2
#define CLOUDVOL_MAP_SIZE   1024

typedef struct CloudVolumeLayer {
	float base, top;       /* m above the reference radius */
	float density;         /* extinction (1/m) at full density */
	float tint[3];         /* times the map's albedo (linear) */
	float coverage;        /* times the map's cover */
	float scale;           /* m: the shape noise's period */
	float sweep;           /* east-west stretch of the shapes */
	float shear;           /* shapes lean east with height (m per m) */
	float erosion;         /* detail noise eating the edges, 0..1 */
} CloudVolumeLayer;

typedef struct CloudVolumeScene {
	int body;              /* the scene's owner; a change resets the history */
	dvec3 center;          /* world, m */
	double radius;         /* reference radius, m */
	dmat4 rotation;        /* body -> world, rotation only */
	/* Weather map frame, body-fixed: its centre on the reference sphere
	   (unit), east and north there, and its width (m). */
	dvec3 mapCenter, mapEast, mapNorth;
	double mapExtent;
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
	float fade;            /* overall density, 0..1 (the provider's hand-off) */
} CloudVolumeScene;

/* Quality and look shared by every body (debug panel). */
typedef struct CloudVolumeParams {
	int enabled;
	int resDivisor;        /* march at 1/n of the screen per axis */
	int viewSteps;         /* max samples per view ray */
	int lightSteps;        /* samples toward the Sun */
	float maxDistance;     /* m: the march ends here (fading out before) */
	float history;         /* weight of the accumulated frames, 0..0.98 */
	float detailScale;     /* detail noise period / shape noise period */
	float multiScatter;    /* strength of the multiple-scattering octaves */
	float powder;          /* darkening of thin cloud facing the Sun */
	float ambient;         /* skylight strength */
} CloudVolumeParams;
CloudVolumeParams *CloudVolume_Params(void);

/* How far the march reaches (m): the Max distance setting, or farther
   when the camera is high above the layers (aboveLayers: its height over
   their lowest base, m), but within the weather map. The clouds fade out
   over the last 40%; a provider fading anything into them uses the same. */
double CloudVolume_Reach(double aboveLayers, double mapExtent);

/* Programs, noise volumes and the weather map (GL up). */
void CloudVolume_Init(void);
/* The main pass's depth (render.c, on a resize). */
void CloudVolume_SetSceneDepth(GLuint depthTex, int samples, float nearPlane);

/* Whether the weather map must be re-baked for this frame: a new body, or
   the camera's ground point more than an eighth of the map from its
   centre. cameraBody: the camera in body-fixed axes (m). */
int CloudVolume_NeedsBake(int body, dvec3 cameraBody, double radius, double extent);
/* Binds the weather map as the render target (both textures, viewport
   set). The provider draws one full-screen pass writing locations 0
   (weather) and 1 (colour). End restores the caller's framebuffer and
   viewport and records which body / centre the map is for. */
void CloudVolume_BeginWeatherBake(void);
void CloudVolume_EndWeatherBake(int body, dvec3 mapCenter);
/* Drops the map (it no longer matches any provider). */
void CloudVolume_Invalidate(void);

/* Marches s and blends it over the bound target (the main pass, after the
   scene's own air). Nothing when disabled or s->fade is 0. */
void CloudVolume_Draw(const FrameContext *f, const CloudVolumeScene *s);

struct nk_context;
/* The "Volume" group of the Clouds tab. */
void CloudVolume_DebugPanel(struct nk_context *ctx);

#endif
