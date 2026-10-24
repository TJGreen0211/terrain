#include "world/terrain/terrain_draw.h"
#include "world/terrain/terrain_gen.h"
#include "world/terrain/terrain_detail.h"
#include "world/ocean/water.h"
#include "world/ocean/water_draw.h"
#include "world/shadows/shadows.h"
#include "world/atmosphere/atmosphere.h"
#include "world/rings/rings.h"
#include "world/clouds/clouds.h"
#include "world/bodies/solar_system.h"
#include "engine/gfx/shader.h"
#include "engine/platform/window.h"
#include "world/WorldConfig.h"
#include <math.h>
#include <stdio.h>

/* The height field lives on the unit sphere: a screen pixel at surface
   distance dist covers dist*thetaPixel world units, i.e. dist*thetaPixel/r
   radians on the sphere, while one face-centre texel covers
   TerrainGen_MapTexelRad() radians. With thetaPixel = 45deg/H that gives
   lod = log2(dist * LOD_K/(r*H)) plus the grazing and face-corner
   corrections in the shaders (planetmap.glsl). */
static GLuint planetHeightTex;

GLuint TerrainDraw_HeightTex(void)
{
	return planetHeightTex;
}

void TerrainDraw_SetHeightTex(GLuint tex)
{
	planetHeightTex = tex;
}
#define LOD_K_NUMER ((float)(0.7853982 / TerrainGen_MapTexelRad()))
/* Geometric relief (quad.vert / depth.vert displace the unit sphere by it)
   is TerrainGen_Style()->landRelief / oceanRelief (terrain_gen.h). Sea level is
   radius 1.0 for both the planet and the water shell; the squared shelf
   profile keeps vertices next to the coast near sea level, so
   coast-straddling triangles barely sag. */

static GLuint surfaceProgram, prepassProgram;

/* The planet surface program. depthPrepass = 1 builds the depth-only
   variant (surface depth with the seafloor pushed behind the water); the
   colour variant never writes depth, so the GPU keeps early-Z for it (see
   quad.frag). */
static GLuint initQuadShader(int depthPrepass) {
	GLuint shader;
	/* Per-pixel terrain detail octaves (detail.glsl) are compiled in or out:
	   even a skipped loop costs the large planet shader registers. */
	char defines[128];
	snprintf(defines, sizeof defines, "%s#define DETAIL_PIXEL_OCTAVES %d\n",
	         depthPrepass ? "#define DEPTH_PREPASS\n" : "", TERRAIN_DETAIL_PIXEL_OCTAVES);
	Shader_CreateDefines(&shader, "world/terrain/quad.vert", "world/terrain/quad.frag", defines);
	return shader;
}

void TerrainDraw_Init(void)
{
	surfaceProgram = initQuadShader(0);
	prepassProgram = initQuadShader(1);
}

GLuint TerrainDraw_SurfaceProgram(void) { return surfaceProgram; }

/* quad.vert's relief profile (body radii) for a raw height h. Monotonic in
   h, so a height range maps straight to a relief range. */
static double reliefOf(double h)
{
	double sea = TerrainGen_GenParams()->seaLevel;
	double s = fmin(fmax((sea - h) / fmax(TerrainGen_GenParams()->oceanDepth, 1e-4), 0.0), 1.0);
	return TerrainGen_Style()->landRelief * fmax(h - sea, 0.0) / fmax(1.0 - sea, 1e-3)
	     - TerrainGen_Style()->oceanRelief * s * s;
}

/* quad.vert / depth.vert's lodCellK: the height-map mip a vertex at
   distance d samples is log2(d * lodCellK), the mip of the widest leaf the
   main walk can have there (half-width d / (2 K R) cube units, K from
   QT_TARGET_TRIANGLE_PX; a face-centre node is ~2 R x its half-width
   across). A function of distance, not of the patch's level, so the
   surface has no height steps where patch levels meet (they showed through
   the atmosphere's depth-clipped haze), and it is never finer than the old
   per-level mip. */
static double lodCellKFor(double radius)
{
	return 1.0 / (TerrainGen_MapTexelRad() * QuadTree_SplitK(QT_TARGET_TRIANGLE_PX) * radius);
}

void TerrainDraw_ViewRange(const Body *planet, dvec3 cam, double *nearest, double *farthest)
{
	dvec3 rel = dsubvec3(cam, planet->position);
	double d = dlengthvec3(rel), R = planet->radius;
	float gmin, gmax;
	TerrainGen_GlobalRange(&gmin, &gmax);
	double topGlobal = R * (1.0 + reliefOf(gmax));
	double cap = d > topGlobal ? acos(R / d) + acos(R / topGlobal) : M_PI;
	/* Body-local nadir: rotation is body -> world, so its transpose. */
	double w[3] = { rel.x / d, rel.y / d, rel.z / d }, dl[3];
	const double (*m)[4] = planet->rotation.m;
	for (int j = 0; j < 3; j++)
		dl[j] = m[0][j] * w[0] + m[1][j] * w[1] + m[2][j] * w[2];
	float lo, hi;
	TerrainGen_HeightRange(dl, fmin(cap, M_PI), 6.0, &lo, &hi);
	double top = R * (1.0 + fmax(reliefOf(hi), 0.0));
	*nearest = fmax(d - top, 0.0);
	*farthest = sqrt(fmax(d * d - R * R, 0.0)) + sqrt(fmax(top * top - R * R, 0.0));
}

double TerrainDraw_GroundRadius(const Body *planet, dvec3 worldDir, double capAngle)
{
	double w[3] = { worldDir.x, worldDir.y, worldDir.z }, dl[3];
	const double (*m)[4] = planet->rotation.m;
	for (int j = 0; j < 3; j++)
		dl[j] = m[0][j] * w[0] + m[1][j] * w[1] + m[2][j] * w[2];
	float lo, hi;
	TerrainGen_HeightRange(dl, capAngle, -HUGE_VAL, &lo, &hi);   /* finest mip */
	/* Seas: the ground is never below the water surface. */
	double relief = reliefOf(hi);
	if (SolarSystem_HasWater(SolarSystem_ActiveBody()))
		relief = fmax(relief, 0.0);
	return planet->radius * (1.0 + relief + TerrainDetail_Amplitude(planet->radius));
}

double Earth_GroundRadius(dvec3 worldDir, double capAngle)
{
	return TerrainDraw_GroundRadius(Bodies_Get(BODY_EARTH), worldDir, capAngle);
}

/* QuadTree_ReliefFn for the active planet's land + seafloor (quad.vert, depth.vert):
   the terrain the shaders can place over the cap, same mip selection (the
   coarsest any vertex there uses, at maxDist: the pyramid range at a mip
   bounds every finer sample). */
static void terrainReliefBounds(const double dir[3], double capAngle, double cell,
                                double maxDist, double *lo, double *hi)
{
	(void)cell;
	double R = Bodies_Get(SolarSystem_ActiveBody())->radius;
	double lodK = LOD_K_NUMER / (R * Window_Height());
	/* Unclamped: HeightRange adds the face-corner bias, then clamps. */
	double lodCell = log2(fmax(maxDist * lodCellKFor(R), 1e-30));
	double lodPix = log2(fmax(maxDist * lodK, 1e-30));
	double lod = fmin(fmax(lodCell, lodPix), TerrainGen_MapMaxLod());
	float hmin, hmax;
	TerrainGen_HeightRange(dir, capAngle, lod, &hmin, &hmax);
	*lo = reliefOf(hmin);
	*hi = reliefOf(hmax);
}

/* It picks the fragments' mip as the pixel term + the grazing term
   (graz >= 0.15) + the face-corner term (added by HeightRange); a coarser
   block's minimum bounds every finer sample. */
int TerrainDraw_FootprintAllLand(const double dir[3], double capAngle, double maxDist)
{
	double lodK = LOD_K_NUMER / (Bodies_Get(SolarSystem_ActiveBody())->radius * Window_Height());
	double fragLod = log2(fmax(maxDist * lodK, 1e-30)) + log2(1.0 / 0.15);
	float hmin, hmax;
	TerrainGen_HeightRange(dir, capAngle, fmin(fragLod, TerrainGen_MapMaxLod()), &hmin, &hmax);
	return hmin > TerrainGen_GenParams()->seaLevel;
}

QuadTree_Body TerrainDraw_Body(const Body *planet, dvec3 cam, QuadTree_ReliefFn relief)
{
	float gmin, gmax;
	TerrainGen_GlobalRange(&gmin, &gmax);
	QuadTree_Body b = { Body_ModelMatrix(planet, cam), planet->radius, relief, reliefOf(gmin) };
	return b;
}

QuadTree_Body TerrainDraw_PlanetBody(const Body *planet, dvec3 cam)
{
	QuadTree_Body b = TerrainDraw_Body(planet, cam, terrainReliefBounds);
	/* Land carries detail noise (quad.vert, depth.vert): widen its
	   bounds and skirts. The water shell does not. */
	b.detailAmp = TerrainDetail_Amplitude(planet->radius);
	b.detailSeam = TerrainDetail_Seam(planet->radius);
	return b;
}

/* Reproduced from the old drawTree so the planet and water programs render
   identically (samplers their shaders do not declare resolve to -1 and are
   ignored). QuadTree_DrawSphere owns the walk, instance buffer, and
   instanced draw. The shaders subtract bodyCenter wherever they need
   planet-local positions (surface up, atmosphere shell). */
void TerrainDraw_SetBodyUniforms(GLuint shader, float lodRadius, mat4 m, const FrameContext *f,
                                 GLuint tex0, GLuint tex2, GLuint slopeTex, GLuint dispTex,
                                 GLuint jacobTex, GLuint skyTex) {
	glUseProgram(shader);

	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &f->view.m[0][0]);

	/* Camera-relative space: the camera is exactly the origin. */
	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "bodyCenter"), m.m[0][3], m.m[1][3], m.m[2][3]);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), f->lightPos.x, f->lightPos.y, f->lightPos.z);
	glUniform1f(glGetUniformLocation(shader, "oceanOffset"), Water_Params()[0]);
	glUniform1f(glGetUniformLocation(shader, "waveAmp"), Water_Params()[1]);
	glUniform1f(glGetUniformLocation(shader, "waveNormalStrength"), Water_Params()[2]);
	/* Per cascade: patches per body radius at its drawn size, and that
	   size over true size (heights scale with it). water.c. */
	float tilings[OCEAN_CASCADES], scales[OCEAN_CASCADES], shoreDepths[OCEAN_CASCADES];
	for (int c = 0; c < OCEAN_CASCADES; c++) {
		scales[c] = (float)Water_CascadeScale(c);
		tilings[c] = (float)(lodRadius / (Water_CascadePatchM(c) * scales[c]));
		/* Water depth (m) each cascade's waves fade out over toward the
		   coast (seastate.glsl): a few of its drawn rms heights, about
		   where waves that size break. */
		shoreDepths[c] = (float)(WAVE_SHORE_RMS * OCEAN_SWELL_RMS_M * Water_CascadeRmsFrac(c)
		                         * scales[c] * Water_Params()[1]);
	}
	glUniform1fv(glGetUniformLocation(shader, "waveTilings"), OCEAN_CASCADES, tilings);
	glUniform1fv(glGetUniformLocation(shader, "waveCascadeScales"), OCEAN_CASCADES, scales);
	glUniform1fv(glGetUniformLocation(shader, "waveShoreDepths"), OCEAN_CASCADES, shoreDepths);
	glUniform1f(glGetUniformLocation(shader, "waveChoppy"), Water_Params()[4]);
	glUniform1f(glGetUniformLocation(shader, "time"), (float)(f->time/100.0));
	/* The wave maps (tex0, slopeTex, dispTex, jacobTex) are 2D arrays, one
	   layer per cascade; the planet passes 0 for them. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, tex0);
	glUniform1i(glGetUniformLocation(shader, "waveHeight"), 0);
	Shadows_BindMaps(shader, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, tex2);
	glUniform1i(glGetUniformLocation(shader, "noiseTexture"), 2);
	glUniform1i(glGetUniformLocation(shader, "detailNormal"), 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D_ARRAY, slopeTex);
	glUniform1i(glGetUniformLocation(shader, "waveSlope"), 3);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D_ARRAY, dispTex);
	glUniform1i(glGetUniformLocation(shader, "waveDisp"), 4);
	glActiveTexture(GL_TEXTURE7);
	glBindTexture(GL_TEXTURE_2D_ARRAY, jacobTex);
	glUniform1i(glGetUniformLocation(shader, "waveJacobian"), 7);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_CUBE_MAP, skyTex);
	glUniform1i(glGetUniformLocation(shader, "skybox"), 5);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_CUBE_MAP, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*f->viewportH));
	glUniform1f(glGetUniformLocation(shader, "lodCellK"), (float)lodCellKFor(lodRadius));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	/* World units are metres, so the body radius is its radius in metres
	   (water.frag sizes the sub-pixel wind waves from it). */
	glUniform1f(glGetUniformLocation(shader, "planetRadiusM"), lodRadius);
	/* Sea level + ocean depth from the last bake. planet and water go through
	   this one function so they can never disagree — a different seaLevel in
	   water.frag would drift the discard edge off the drawn coastline. */
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), TerrainGen_GenParams()->seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), TerrainGen_GenParams()->oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), TerrainGen_Style()->landRelief);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), TerrainGen_Style()->oceanRelief);
	/* quad.frag rewrites gl_FragDepth for seafloor that pokes through the
	   water; it needs the main projection's convention and near plane, and
	   how far under sea level the wave troughs can reach. */
	glUniform1f(glGetUniformLocation(shader, "depthNear"), f->nearPlane);
	glUniform1f(glGetUniformLocation(shader, "seaClearance"), (float)Water_Excursion(lodRadius));
	glUniform1f(glGetUniformLocation(shader, "seaSagK"), (float)Water_SagK(lodRadius));
	/* Climate map (unit 8) + surface style for quad.frag's biome shading. */
	TerrainGen_SetSurfaceUniforms(shader, 8);
	/* Terrain detail (quad.vert/quad.frag), faded for the main walk. */
	TerrainDetail_SetUniforms(shader, lodRadius, QT_TARGET_TRIANGLE_PX);
	glActiveTexture(GL_TEXTURE0);
}

void TerrainDraw_ShadowCasters(const QuadTree_Body *body, mat4 lightSpace, double minSpacing,
                               double trianglePx, int cullNear, const FrameContext *f) {
	GLuint shader = Shadows_DepthProgram();
	mat4 m = dmat4tomat4(body->model);
	float lodRadius = (float)body->radius;
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 1);
	glUniform1f(glGetUniformLocation(shader, "displace"), 1.0f);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "lightSpace"), 1, GL_FALSE, &lightSpace.m[0][0]);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_CUBE_MAP, planetHeightTex);
	glUniform1i(glGetUniformLocation(shader, "heightMap"), 6);
	glActiveTexture(GL_TEXTURE0 + TERRAIN_SURFACE_UNIT);
	glBindTexture(GL_TEXTURE_CUBE_MAP, TerrainGen_GenParams()->surfaceTex);
	glUniform1i(glGetUniformLocation(shader, "surfaceMap"), TERRAIN_SURFACE_UNIT);
	glActiveTexture(GL_TEXTURE0);
	glUniform1f(glGetUniformLocation(shader, "seaLevel"), TerrainGen_GenParams()->seaLevel);
	glUniform1f(glGetUniformLocation(shader, "oceanDepth"), TerrainGen_GenParams()->oceanDepth);
	glUniform1f(glGetUniformLocation(shader, "landRelief"), TerrainGen_Style()->landRelief);
	glUniform1f(glGetUniformLocation(shader, "oceanRelief"), TerrainGen_Style()->oceanRelief);
	/* Same vertex lod inputs as TerrainDraw_SetBodyUniforms, so the
	   displaced caster matches the drawn surface vertex for vertex. */
	glUniform1f(glGetUniformLocation(shader, "lodK"), LOD_K_NUMER/(lodRadius*f->viewportH));
	/* The main walk's mip by distance (not the caster walk's), so casters
	   sample the very heights the receivers do. */
	glUniform1f(glGetUniformLocation(shader, "lodCellK"), (float)lodCellKFor(lodRadius));
	glUniform3f(glGetUniformLocation(shader, "lodCameraPos"), 0.0f, 0.0f, 0.0f);
	/* Terrain detail faded for this walk's triangles. */
	TerrainDetail_SetUniforms(shader, lodRadius, trianglePx);
	QuadTree_DrawShadowCasters(body, lightSpace, minSpacing, trianglePx, cullNear);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
}

/* Drawn before the water so the translucent water can blend over the
   seafloor. Depth prepass, then colour: the prepass lays down the surface
   depth (seafloor pushed behind the water) with a cheap program; the full
   shader then runs only where its fragment is the visible one (depth test
   >=, no depth writes, so early-Z holds). The second draw reuses the walk. */
void TerrainDraw_Planet(const QuadTree_Body *body, const FrameContext *f, float atmoInner,
                        int bodyId, GLuint noiseTex)
{
	float lodRadius = (float)body->radius;
	mat4 model = dmat4tomat4(body->model);
	TerrainDraw_SetBodyUniforms(prepassProgram, lodRadius, model, f,
	                            0, noiseTex, 0, 0, 0, 0);
	/* Per MSAA sample: a shader that writes gl_FragDepth writes one
	   value for the whole pixel, and the colour pass's per-sample >=
	   test would then fail on about half the samples of every sloped
	   surface (resolving to see-through terrain). The prepass is cheap,
	   so running it per sample costs little. */
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glEnable(GL_SAMPLE_SHADING);
	glMinSampleShading(1.0f);
	QuadTree_DrawSphere(body, f->viewProj);
	glDisable(GL_SAMPLE_SHADING);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	TerrainDraw_SetBodyUniforms(surfaceProgram, lodRadius, model, f,
	                            0, noiseTex, 0, 0, 0, 0);
	TerrainGen_SetOceanUniforms(surfaceProgram, 10);   /* beach swash exposure */
	Atmosphere_SetExtinction(surfaceProgram, atmoInner, Atmosphere_Params());
	Rings_BindReceiver(surfaceProgram);
	Clouds_BindReceiver(surfaceProgram, bodyId);
	glDepthFunc(GL_GEQUAL);   /* reversed-Z: nearer = greater */
	glDepthMask(GL_FALSE);
	QuadTree_DrawSphere(body, f->viewProj);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_GREATER);
}
