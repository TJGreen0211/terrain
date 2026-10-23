#ifndef GAS_GIANT_H
#define GAS_GIANT_H

#include "engine/core/gl.h"

/* Gas giant looks (docs/gas-giants.md), drawn on the planet stand-in
   (world/bodies/planets.c, planet_impostor.frag through gasgiant.glsl).

   Each giant has a GasGiantStyle generated from a seed: a family, a warm
   painterly palette, a band layout, turbulence, storms and a haze. The
   band layout is built on the CPU into a 1D latitude profile (RGB band
   colour, A turbulence weight), rebuilt whenever the style changes. The
   shader spins the profile lookup around the storms and polar cyclones,
   warps it with noise into streamers with bright wisps, then lights it
   through the haze (limb darkening, wrap light at the terminator, a
   forward-scattering veil and a glow beyond the limb). A sparse layer of
   high clouds sits above, in a few latitude bands, and casts soft shadows
   on the deck. */

#define GASGIANT_MAX_STORMS 10
#define GASGIANT_MAX_POLAR  8
#define GASGIANT_MAX_HIGH   4

typedef enum GasGiantFamily {
	GASGIANT_JOVIAN,       /* cream zones, rust and umber belts, a red spot */
	GASGIANT_SATURNIAN,    /* muted butterscotch, soft bands, thick haze */
	GASGIANT_ICE,          /* teal-grey, few broad bands, dark spots */
	GASGIANT_EXOTIC,       /* one free hue, kept harmonious */
	GASGIANT_FAMILY_COUNT
} GasGiantFamily;

typedef struct GasGiantStorm {
	float lat, lon;        /* rad, body-fixed */
	float size;            /* half-length east-west, rad */
	float aspect;          /* east-west / north-south */
	float spin;            /* swirl turns at the centre, signed */
	float color[3];        /* display colour */
	float opacity;         /* how much the storm colour covers the bands */
} GasGiantStorm;

typedef struct GasGiantStyle {
	int family;            /* GasGiantFamily */
	unsigned int seed;
	int bandCount;         /* bands between the polar caps */
	float bandJitter;      /* 0 = even widths */
	float poleLat;         /* deg, where the polar cap starts */
	float zone[3];         /* palette, display colours */
	float belt[3];
	float pole[3];
	float accent[3];       /* tints some belts and the big storms */
	float contrast;        /* zone / belt separation */
	float edgeSharpness;   /* 0 soft .. 1 sharp band edges */
	float fineBands;       /* thin streaks inside each band */
	float turbulence;      /* swirl strength at band edges */
	float streak;          /* east-west stretch of the swirls */
	float detail;          /* cloud texture brightness variation */
	float wisps;           /* bright cirrus-like strands along the streamers */
	float relief;          /* sculpted shading of the cloud texture */
	float hazeTau;         /* veil optical depth (straight down) */
	float hazeColor[3];
	float shellFraction;   /* haze shell / radius: the glow beyond the limb */
	int stormCount;
	GasGiantStorm storms[GASGIANT_MAX_STORMS];
	/* Polar cyclones: a ring around each pole plus one on it. */
	int polarCount;        /* in the ring (0 = only the pole's; -1 = none) */
	float polarLat;        /* deg, the ring's latitude */
	float polarSize;       /* rad */
	float polarSpin;       /* swirl at the centre, rad */
	float polarColor[3];   /* their cores (display) */
	/* High clouds: bright streaks and plumes above the deck. */
	float high;            /* amount (0 = none) */
	float highHeight;      /* altitude, radii: sets the shadow offset */
	float highColor[3];    /* display */
	int highCount;         /* latitude bands they gather in */
	float highBand[GASGIANT_MAX_HIGH][3];   /* lat (deg), half-width (deg), strength */

	/* Built from the fields above; keep these last (the rebuild check
	   hashes everything before profileTex). */
	GLuint profileTex;     /* RGBA: band colour, turbulence weight */
	unsigned int profileHash;
} GasGiantStyle;

/* Startup (GL up): builds the shared noise texture. */
void GasGiant_Init(void);

/* Look shared by every giant (debug panel). */
typedef struct GasGiantParams {
	float limbK;           /* Minnaert exponent: > 1 darker limb */
	float wrap;            /* light wrapping past the terminator */
	float terminator[3];   /* warm tint of the light near the terminator */
	float ambient;         /* night-side haze light */
	float rim;             /* forward-scatter strength of the haze */
	float glow;            /* glow beyond the limb */
	float exposure;
} GasGiantParams;
GasGiantParams *GasGiant_Params(void);

/* A new style from seed. aAU: the orbit (farther out: likelier ice
   giant). Frees nothing: call GasGiant_Free on a style that was bound. */
void GasGiant_Generate(GasGiantStyle *s, unsigned int seed, double aAU);
/* Drops the style's profile texture (GL context current). */
void GasGiant_Free(GasGiantStyle *s);
/* Area-weighted mean of the band colours: the dot / far colour. */
void GasGiant_MeanColor(const GasGiantStyle *s, float out[3]);
const char *GasGiant_FamilyName(int family);

/* Once per stand-in program at startup: points gasProfile at its own unit
   and switches the gas shading off. */
void GasGiant_AttachStandIn(GLuint shader);
/* Program in use: s's uniforms and profile (rebuilt if s changed), or
   gas shading off when s is NULL. inside: how much of the haze the
   interior pass draws instead (gas_interior.h; 0 from outside). radius,
   altitude: the giant's and the camera's above its cloud tops (m). Leaves
   glActiveTexture at unit 0. */
void GasGiant_BindStandIn(GLuint shader, GasGiantStyle *s, double radius, double altitude);

struct nk_context;
/* The Gas giants tab: the focused giant's style and the shared look. */
void GasGiant_DebugPanel(struct nk_context *ctx);

#endif
