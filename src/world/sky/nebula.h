#ifndef NEBULA_H
#define NEBULA_H

#include "engine/core/gl.h"
#include "engine/math/linalg.h"

/* The nebula (.kilo/plans/1792611923000-volumetric-nebula-skybox.md): one
   density function (shaders/nebula.glsl) in nebula units n = (p - sun) /
   NEBULA_RADIUS_M, in two layers that never overlap:

   - far: a shell |n| in [NEBULA_FAR_START, NEBULA_FAR_END], baked into a
     cubemap as seen from the sun with stars at infinity behind it
     (Nebula_BakeSky). `cube.exe --bake-sky <name>` writes it to
     assets/textures/nebula_<name>/ with the settings in nebula_<name>.txt;
     the skybox loads it when the sky name is set (skybox.c).
   - near: wisps inside |n| < 1, ray-marched every frame (Nebula_Draw) with
     real parallax, ended at the scene depth.

   Settings: Nebula_Params() (debug panel). The near layer follows them live (its
   presence grid is rebuilt on change); the far layer on Rebake. */

typedef struct nebulaParams {
	unsigned int seed;
	/* Near layer (nebula units). Wavelength: the ridged noise's largest
	   octave; clumps are ~4x that. Sigma: extinction per unit density per
	   nebula unit. */
	float nearWavelength;
	float nearCoverage;     /* fraction of the volume in clumps, 0..1 */
	float nearDensity;      /* density scale */
	float nearSigma;
	/* Far layer (shell units: |n| / NEBULA_FAR_START, the shell is
	   1 .. FAR_END / FAR_START). */
	float farWavelength;
	float farCoverage;
	float farDensity;
	float farSigma;
	/* Shared look. */
	float turbulence;       /* domain warp, wavelengths */
	float contrast;         /* filament sharpness (pow of the ridges) */
	float emission;         /* glow per unit density per unit length */
	float dark;             /* 0 = all gas glows, 1 = wide dark lanes */
	float colA[3];          /* palette ends (linear): teal .. amber */
	float colB[3];
	float band;             /* galaxy band: 0 = none, 1 = gas only near it */
	float bandTilt;         /* deg: band plane from the ecliptic (world XZ) */
	float starDensity;      /* 0..1 (bake) */
	float starBrightness;
	float scatter;          /* near layer: forward-scattered sunlight */
	/* Far layer lighting (lit dust, sky bake). Lights: seeded bright
	   cores in the shell, alternately colB / colA tinted; gas clumps
	   around them. Radius in shell units. */
	float lightCount;       /* 0..8, one per body (a float for the file) */
	float lightIntensity;
	float lightRadius;
	float lightClump;       /* coverage added at a light, 0..1 */
	float dustAlbedo;
	float ambient;          /* shadow fill, colA tinted */
	float phaseG;           /* forward-scatter lobe */
	float powder;
	float coreSize;         /* visible cores, radians */
	/* Far layer shapes (Nubis: Perlin-Worley base, Worley erosion). */
	float farErosion;       /* 0 = smooth blobs, 1 = deeply eroded edges */
	float farDetail;        /* erosion detail repeats per wavelength */
	/* Far layer composition: seeded cloud bodies (stretched ellipsoids in
	   the shell, toward the band); lights sit in the first lightCount. */
	float bodyCount;        /* 0..8 */
	float bodySize;         /* long half-axis, shell units */
	float bodyStretch;      /* long / short axis */
	float bodyBreakup;      /* edge noise, body radii */
	/* Far layer emissive gas: glows only, in a wider envelope than the
	   dust, brightest near the lights. */
	float gasStrength;      /* base glow, palette coloured */
	float gasLight;         /* glow near lights, per unit light colour */
	float gasExtent;        /* envelope size over the bodies' */
	float gasRadius;        /* light falloff for the glow, shell units */
	float gasContrast;      /* patchy (high) vs even (low) */
	float gasBack;          /* envelope pushed behind the dust, shell units */
	/* Light placement: how far behind its body (as seen from the sun) each
	   light sits, in the body's depth along that line: < 1 inside the
	   dust, > 1 behind it (backlit: lit rims, dark masses). */
	float lightBack;
	/* Glow pass (sky bake): mip-sum bloom, exposure, contrast S-curve. */
	float bloomStrength;
	float bloomLevels;      /* mips summed (a float for the settings file) */
	float skyExposure;
	float skyContrast;      /* 1 = none */
	/* Cloud filaments: streamers stretched along each body's long axis,
	   reaching out of it (cloud-like, billowy edges, not hair). */
	float filament;         /* 0 = none, 1 = strong */
	float filReach;         /* envelope along the long axis, body radii */
	float filStretch;       /* length / width */
	float filScale;         /* strands across a body */
	float filBend;          /* sideways sway */
	/* Luminous edges: thin dust at cloud boundaries glows in the nearby
	   lights' colour. */
	float edgeGlow;
	float edgeWidth;        /* fraction of full density counted as edge */
	/* Cloud interiors: coverage cap (< 1 leaves folds and holes in the
	   cores) and a density exponent (> 1: dense cores vs thin gas). */
	float bodyFill;
	float densityCurve;
	/* Strands: coverage cap (< 1: chains of puffs, not filled tubes) and
	   tip taper (0 = none). */
	float filCover;
	float filTaper;
} nebulaParams;

/* The live settings (debug panel; a baked sky set loads its own). */
nebulaParams *Nebula_Params(void);
/* Runtime near-layer pass on/off (starts at NEBULA_ENABLE_DEFAULT; debug
   panel). */
int Nebula_Enabled(void);
/* Sky set name: assets/textures/nebula_<name>/ ("" = skybox3). Starts at
   NEBULA_SKY_NAME, or --sky <name>. Also where "Save sky" writes. */
const char *Nebula_SkyName(void);
nebulaParams Nebula_Defaults(void);

/* Command line: --bake-sky <name> [--sky-size <n>] [--sky-axes]
   [--sky-params <set>: settings from nebula_<set>.txt]
   [--sky-seed <n>: overrides the seed], and
   --sky <name>, --nebula (in-system pass on). Before the window exists. */
void Nebula_ParseArgs(int argc, char *argv[]);
int Nebula_BakeRequested(void);
/* The --bake-sky run, after GL is up: bake, write, return the exit code. */
int Nebula_RunBake(void);

/* Programs, noise lattice, presence grid. After GL is up. */
void Nebula_Init(void);

/* The main pass's depth (main.c's scene target), as for the dome. */
void Nebula_SetSceneDepth(GLuint depthTex, int samples, float nearPlane);

/* Loads nebula_<name>.txt into Nebula_Params(); 0 if absent. */
int Nebula_LoadParams(const char *name);

/* Per frame, before the scene: rebuilds the presence grid when the near
   layer's settings change, and runs Rebake / Save requests (blocking). */
void Nebula_Update(void);
void Nebula_RequestRebake(int axisDebug);
void Nebula_RequestSave(void);

/* The sky to draw: the in-memory bake once there is one, else fallback. */
GLuint Nebula_SkyTexture(GLuint fallback);

/* The near layer over the bound scene target, after the rings and before
   the atmosphere dome. Premultiplied, no depth test or writes; the caller
   masks the alpha channel (the cloud cover). view: rotation only. */
void Nebula_Draw(mat4 proj, mat4 view, dvec3 cam);

struct nk_context;
/* The Nebula tab of the debug panel (nebula.c). */
void Nebula_DebugPanel(struct nk_context *ctx);

#endif
