#ifndef TONE_MAP_H
#define TONE_MAP_H
#include "engine/core/gl.h"

struct nk_context;

/* The scene's tone map settings, shared by every shader that includes
   engine/gfx/tonemap.glsl through one uniform block (ToneMapBlock), which
   Shader_Create binds to TONEMAP_BINDING. See docs/hdr.md.

   The scene target holds linear radiance; the resolve, per sample: white
   balance -> exposure -> curve -> grading, then averages and dithers. */

#define TONEMAP_BINDING 0

typedef enum ToneMapCurve {
	TONEMAP_ACES,       /* Narkowicz ACES fit (sceneFromDisplay inverts it) */
	TONEMAP_NEUTRAL,    /* Khronos PBR Neutral: keeps hue and saturation */
	TONEMAP_AGX,        /* AgX (minimal fit): soft, desaturates highlights */
	TONEMAP_REINHARD,   /* extended Reinhard, per channel, with a white point */
	TONEMAP_CURVE_COUNT
} ToneMapCurve;

typedef enum ToneMapDebug {
	TONEMAP_DEBUG_OFF,
	TONEMAP_DEBUG_INVALID,   /* NaN/Inf magenta, near fp16 max cyan, negative yellow */
	TONEMAP_DEBUG_EV,        /* false colour of scene luminance in stops */
	TONEMAP_DEBUG_COUNT
} ToneMapDebug;

typedef struct ToneMapParams {
	float exposure;     /* scale before the curve */
	int curve;          /* ToneMapCurve */
	float whitePoint;   /* Reinhard: linear value that maps to white */
	float temperature;  /* white balance, -1 cool .. +1 warm (linear, before the curve) */
	float tint;         /* -1 green .. +1 magenta */
	float saturation;   /* after the curve, 1 = unchanged */
	float contrast;     /* after the curve, around mid grey, 1 = unchanged */
	float lift[3];      /* after the curve: raises blacks, 0 = unchanged */
	float gamma[3];     /* mid-tones, 1 = unchanged (> 1 brighter) */
	float gain[3];      /* scales, 1 = unchanged */
	int dither;         /* triangular +-1 LSB on the 8-bit output */
	int debugView;      /* ToneMapDebug */
} ToneMapParams;

void ToneMap_Init(void);   /* after GL is up, before anything draws */
ToneMapParams *ToneMap_Params(void);
/* Curve, white balance and grading back to neutral (keeps exposure). */
void ToneMap_ResetGrading(void);
/* Uploads the params to the uniform block; once per frame before drawing. */
void ToneMap_Update(void);
void ToneMap_DebugPanel(struct nk_context *ctx);

#endif
