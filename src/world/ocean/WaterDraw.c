#include "world/ocean/WaterDraw.h"
#include "world/terrain/TerrainDraw.h"
#include "world/terrain/TerrainGen.h"
#include "world/atmosphere/Atmosphere.h"
#include "world/rings/Rings.h"
#include "world/clouds/Clouds.h"
#include "engine/gfx/Common.h"
#include "world/WorldConfig.h"
#include <math.h>

static GLuint waterProgram;

void WaterDraw_Init(void)
{
	Shader_Create(&waterProgram, "world/ocean/water.vert",
		"world/ocean/water.frag");
}

GLuint WaterDraw_Program(void) { return waterProgram; }

/* Each cascade's heights are further scaled by Water_CascadeScale (Water.c),
   with its patches, so slopes (and look) stay the same, only larger. */
double Water_GeomScale(double radius)
{
	return OCEAN_SWELL_RMS_M / (OCEAN_FFT_RMS * radius);
}

/* Water shell bound, in rms of the displaced swell: the sea state can
   double it (water.vert's calm clamp) and peaks stay within ~4 rms.
   Choppy displacement moves vertices sideways by the same scale. */
#define WAVE_BOUND_RMS 8.0

/* The largest cascade scale bounds the sum (the finest stays at 1). */
double Water_Excursion(double radius)
{
	return WAVE_BOUND_RMS * OCEAN_FFT_RMS * Water_GeomScale(radius)
	     * fmax(Water_Params()[3], 1.0)
	     * Water_Params()[1] * (1.0 + Water_Params()[4]);
}

/* QuadTree_ReliefFn for Earth's water shell (water.vert): sea level, plus
   waves wherever there is sea. Its own walk, so the thin shell is culled
   and refined on its own instead of inheriting the seafloor's depth range.
   All-land footprints are empty (water.frag discards every fragment). */
static void waterReliefBounds(const double dir[3], double capAngle, double cell,
                              double maxDist, double *lo, double *hi)
{
	(void)cell;
	if (TerrainDraw_FootprintAllLand(dir, capAngle, maxDist)) {
		*lo = 1.0;
		*hi = 0.0;
		return;
	}
	double shell = Water_Params()[0] - 1.0;
	double wave = Water_Excursion(Bodies_Get(BODY_EARTH)->radius);
	*lo = shell - wave;
	*hi = shell + wave;
}

/* FFT cascades: waveHeight/Disp/Slope arrays + skybox reflection in
   water.frag. water.frag fades to clear in the shallows from the
   per-pixel water depth. */
void WaterDraw_Draw(const waves *ocean, const Body *planet, const FrameContext *f,
                    GLuint skyTex, float atmoInner, int bodyId)
{
	float fScale = (float)planet->radius;
	/* Its own walk and bounds: the thin shell (land footprints culled). */
	QuadTree_Body waterBody = TerrainDraw_Body(planet, f->cam, waterReliefBounds);
	waterBody.trianglePx = QT_WATER_TRIANGLE_PX;
	mat4 model = dmat4tomat4(waterBody.model);

	TerrainDraw_SetBodyUniforms(waterProgram, fScale, model, f,
	                            ocean->dyTexture, 0,
	                            ocean->slopeTexture, ocean->dispTexture, ocean->jacobTexture, skyTex);
	/* Also feeds the sky water.frag reflects (E, G_M: same as the dome). */
	Atmosphere_SetExtinction(waterProgram, atmoInner, Atmosphere_Params());
	glUniform1f(glGetUniformLocation(waterProgram, "waveGeomScale"), (float)Water_GeomScale(planet->radius));
	/* Wind / fetch sea state. Unit 10: 0-9 are taken (Rings uses 9). */
	TerrainGen_SetOceanUniforms(waterProgram, 10);
	Rings_BindReceiver(waterProgram);
	Clouds_BindReceiver(waterProgram, bodyId);
	/* Water.frag's back-face early-out is only safe from outside the
	   shell; the surface view (camera inside) must still see it. */
	double shellCamDist = dlengthvec3(dsubvec3(f->cam, planet->position));
	glUniform1f(glGetUniformLocation(waterProgram, "outsideShell"),
	            shellCamDist > fScale*1.05 ? 1.0f : 0.0f);
	glEnable(GL_BLEND);
	/* Premultiplied: water.frag adds its surface reflection over
	   the seafloor where the column is clear. */
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	QuadTree_DrawSphere(&waterBody, f->viewProj);
	glDisable(GL_BLEND);
}
