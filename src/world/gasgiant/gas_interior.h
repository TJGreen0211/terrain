#ifndef GAS_INTERIOR_H
#define GAS_INTERIOR_H

#include "engine/core/gl.h"
#include "engine/math/linalg.h"
#include "engine/gfx/frame.h"

struct GasGiantStyle;

/* Inside a gas giant (docs/gas-giant-interior.md, stage 1).

   The cloud tops are the stand-in sphere (planets.c, gasgiant.glsl), drawn
   from close up as the far deck. Over them is a haze whose density falls
   off exponentially with height (GasInterior_ScaleHeight), with the same
   column optical depth as the stand-in's own veil (GasGiantStyle.hazeTau),
   and below them a murk that darkens with depth down to the floor
   (GAS_INTERIOR_FLOOR_M). Near the cloud tops a full-screen pass marches
   the haze to the scene depth; higher up the stand-in's veil does the same
   job, and the two cross-fade (GasInterior_StandInHaze). */

typedef struct GasInteriorParams {
	int enabled;
	float ambient;         /* scattered skylight, x the Sun */
	float rays;            /* soft rays around the Sun */
	float glare;           /* the bright aureole right around the Sun */
	float murk;            /* murk density below the cloud tops, x GAS_DECK_MURK_PER_M */
	float lightning;       /* flashes per second around the camera */
	float flash;           /* their brightness, x the Sun */
} GasInteriorParams;
GasInteriorParams *GasInterior_Params(void);

void GasInterior_Init(void);
/* The main pass's depth (render.c, on a resize). */
void GasInterior_SetSceneDepth(GLuint depthTex, int samples, float nearPlane);

/* The gas giant whose clouds the camera is nearest, within the slow-flight
   range (GAS_FLIGHT_FULL_SPEED_ALT_M), or -1; *altitude: above its cloud
   tops (m, negative below them). */
int GasInterior_Body(dvec3 cam, double *altitude);
/* Flight speed factor at this altitude above a giant's cloud tops. */
double GasInterior_SpeedScale(double altitude);
/* Closest the camera may come to giant id's centre (m). */
double GasInterior_FloorRadius(int id);
/* The haze's scale height (m) over a giant of this style and radius. */
double GasInterior_ScaleHeight(const struct GasGiantStyle *s, double radius);
/* Share of the haze the stand-in still draws itself at this altitude, for
   scale height H: 1 above the interior, 0 deep in it (the interior pass
   takes the rest). */
float GasInterior_StandInHaze(double H, double altitude);

/* The haze and murk around the camera, over the whole scene (after the
   Sun's glow). Nothing when the camera is not inside a giant's haze.
   First it makes this frame's cloud layers (re-baking their weather map
   and the clouds' shadow map, ageing the lightning), so the haze shows
   the shafts the clouds cast; GasInterior_DrawClouds draws them. */
void GasInterior_Draw(const FrameContext *f);

/* How much of the volumetric cloud layers show at this altitude, for
   scale height H: 1 among them, 0 well above (the stand-in alone). */
float GasInterior_VolumeFade(double H, double altitude);
/* How far the cloud layers reach from the camera at this altitude (m);
   they fade out over its last 40% (CloudVolume_Reach). radius: the body's. */
double GasInterior_CloudReach(double H, double radius, double altitude);
/* The giant's cloud layers around the camera (world/clouds/cloud_volume),
   as GasInterior_Draw made them this frame (their weather map from the
   exterior's cloud field). Into the main pass, after the haze: the clouds
   carry their own air in front. */
void GasInterior_DrawClouds(const FrameContext *f);

struct nk_context;
/* The "Interior" group of the Gas giants tab. */
void GasInterior_DebugPanel(struct nk_context *ctx);

#endif
