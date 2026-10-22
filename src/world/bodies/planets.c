#include "engine/gfx/shader.h"
#include "engine/gfx/framebuffer.h"
#include "world/bodies/planets.h"
#include "world/bodies/solar_system.h"
#include "world/rings/rings.h"
#include "world/clouds/clouds.h"
#include "world/gasgiant/gas_giant.h"
#include "engine/platform/window.h"
#include "world/WorldConfig.h"
#include <math.h>

/* Look constants (screen pixels / brightness), not scale-dependent. */
#define DOT_RADIUS_PX     1.5    /* below this the disc becomes a dot */
#define DOT_SIGMA_PX      0.7    /* dot's gaussian width */
#define DOT_MIN_FLUX      3.0    /* floor on a dot's total light (sum over pixels) */
#define FULL_MIN_PX       3.0    /* full-detail bodies below this use the stand-in */

static GLuint shader;
static GLuint quadVAO, quadVBO;

void Planets_Init(void)
{
	Shader_Create(&shader, "world/bodies/planet_impostor.vert", "world/bodies/planet_impostor.frag");
	/* ringProfile on the ring unit (it would alias a cloud map's otherwise). */
	Rings_AttachReceiver(shader);
	GasGiant_AttachStandIn(shader);

	/* Unit quad corners, perimeter order for a fan. */
	GLfloat corners[] = { -1.0f, -1.0f,  1.0f, -1.0f,  1.0f, 1.0f,  -1.0f, 1.0f };
	glGenVertexArrays(1, &quadVAO);
	glBindVertexArray(quadVAO);
	glGenBuffers(1, &quadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	GLint loc = glGetAttribLocation(shader, "vCorner");
	glEnableVertexAttribArray(loc);
	glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (GLvoid *)0);
	glBindVertexArray(0);
}

/* Screen pixels per radian at the view centre. */
static double pixelsPerRadian(void)
{
	return 0.5 * Window_Height() / tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0);
}

double Planets_ScreenRadiusPx(const Body *b, dvec3 cam)
{
	double d = dlengthvec3(dsubvec3(b->position, cam));
	if (d <= b->radius)
		return 0.0;
	return asin(b->radius / d) * pixelsPerRadian();
}

int Planets_FullDetail(const Body *b, dvec3 cam)
{
	double d = dlengthvec3(dsubvec3(b->position, cam));
	return d <= b->radius || Planets_ScreenRadiusPx(b, cam) >= FULL_MIN_PX;
}

/* World vector -> view space (Camera_ViewMatrix is a pure rotation whose rows
   are the camera axes), in double. */
static dvec3 toView(mat4 view, dvec3 w)
{
	dvec3 v;
	v.x = view.m[0][0] * w.x + view.m[0][1] * w.y + view.m[0][2] * w.z;
	v.y = view.m[1][0] * w.x + view.m[1][1] * w.y + view.m[1][2] * w.z;
	v.z = view.m[2][0] * w.x + view.m[2][1] * w.y + view.m[2][2] * w.z;
	return v;
}

static dvec3 unit(dvec3 v)
{
	double l = dlengthvec3(v);
	return l > 0.0 ? dscalarvec3(v, 1.0 / l) : v;
}

static void uniformV3(const char *name, dvec3 v)
{
	glUniform3f(glGetUniformLocation(shader, name), (float)v.x, (float)v.y, (float)v.z);
}

/* Lambert sphere's phase function at phase angle a (1 at full). */
static double lambertPhase(double a)
{
	return (sin(a) + (M_PI - a) * cos(a)) / M_PI;
}

/* One body; dots: 0 = disc pass (skips dots), 1 = dot pass (skips discs). */
static void drawBody(int id, mat4 view, dvec3 cam, int dots, int ringBody)
{
	const Body *b = Bodies_Get(id);
	const PlanetInfo *pi = SolarSystem_Info(id);
	dvec3 rel = dsubvec3(b->position, cam);
	double d = dlengthvec3(rel), R = b->radius;
	if (R <= 0.0 || d <= R * 1.0001)
		return;
	double rpx = asin(R / d) * pixelsPerRadian();
	int isDot = rpx < DOT_RADIUS_PX;
	if (isDot != dots)
		return;

	/* Extent around the centre: the sphere, its air and its rings. */
	int gas = pi->type == PLANET_GAS;
	double atmoThick = gas ? pi->gas.shellFraction
	                 : pi->atmo.shellHeight > 0.0f ? pi->atmo.shellHeight / R : 0.0;
	int rings = pi->hasRings;
	double extent = R * fmax(1.0 + atmoThick, rings ? RING_OUTER * 1.02 : 1.0);
	extent = fmin(extent, d / 1.02);   /* inside the rings: they are cut off */

	dvec3 cv = toView(view, rel);
	if (cv.z > extent)
		return;   /* behind the camera */

	/* Billboard half-size: the extent's silhouette cone in the plane through
	   the centre (exact cover), or the dot's footprint. */
	double half = extent * d / sqrt(d * d - extent * extent);
	if (isDot)
		half = fmax(R * d / sqrt(d * d - R * R), 3.0 * DOT_SIGMA_PX * d / pixelsPerRadian());

	dvec3 toSun = unit(dsubvec3(Bodies_Get(BODY_SUN)->position, b->position));
	dvec3 ringN = SolarSystem_RingNormal(id);

	glUniform3f(glGetUniformLocation(shader, "centerView"), (float)cv.x, (float)cv.y, (float)cv.z);
	glUniform1f(glGetUniformLocation(shader, "halfSize"), (float)half);
	glUniform1f(glGetUniformLocation(shader, "radius"), (float)R);
	/* Camera relative to the body, in body radii (o in the ray-sphere). */
	glUniform3f(glGetUniformLocation(shader, "camBody"), (float)(-cv.x / R), (float)(-cv.y / R), (float)(-cv.z / R));
	uniformV3("sunView", toView(view, toSun));
	uniformV3("ringNormalView", toView(view, ringN));
	/* View -> body-fixed axes (body rotation^T * view^T), for the mottling
	   and the gas giants' bands. */
	{
		float m[9];
		for (int i = 0; i < 3; i++)        /* body axis i */
			for (int j = 0; j < 3; j++) {  /* view axis j */
				double s = 0.0;
				for (int k = 0; k < 3; k++)
					s += b->rotation.m[k][i] * view.m[j][k];
				m[j * 3 + i] = (float)s;    /* column j = view axis j in body coords */
			}
		glUniformMatrix3fv(glGetUniformLocation(shader, "viewToBody"), 1, GL_FALSE, m);
	}
	glUniform3f(glGetUniformLocation(shader, "color"), b->color[0], b->color[1], b->color[2]);
	GasGiant_BindStandIn(shader, gas && !isDot ? &SolarSystem_Info(id)->gas : NULL);
	glUniform1f(glGetUniformLocation(shader, "mottle"),
	            pi->type == PLANET_ROCKY || pi->type == PLANET_DWARF ? 1.0f : 0.0f);
	glUniform1f(glGetUniformLocation(shader, "seed"), (float)(pi->ringSeed % 997u) * 0.137f + (float)id);
	glUniform1f(glGetUniformLocation(shader, "atmoThick"), (float)atmoThick);
	glUniform3f(glGetUniformLocation(shader, "atmoColor"), pi->atmo.C_R.x, pi->atmo.C_R.y, pi->atmo.C_R.z);
	glUniform1i(glGetUniformLocation(shader, "ringOn"), rings);
	if (rings && !isDot) {
		glUniform1i(glGetUniformLocation(shader, "ringSheet"), id != ringBody);
		glUniform2f(glGetUniformLocation(shader, "ringRadii"), RING_INNER, RING_OUTER);
		Rings_BindStandIn(shader, pi->ringSeed);
	}
	glUniform1i(glGetUniformLocation(shader, "dotMode"), isDot);
	Clouds_BindStandIn(shader, id);
	/* Pixel size in body radii at the body (edge anti-aliasing). */
	glUniform1f(glGetUniformLocation(shader, "pixelR"), (float)(d / pixelsPerRadian() / R));
	if (isDot) {
		/* The disc's total light: mean Lambert brightness over the disc
		   (2/3 x the phase function) x its area in pixels. */
		double phase = acos(fmax(fmin(-(rel.x * toSun.x + rel.y * toSun.y + rel.z * toSun.z) / d, 1.0), -1.0));
		double flux = (2.0 / 3.0) * lambertPhase(phase) * M_PI * rpx * rpx;
		glUniform1f(glGetUniformLocation(shader, "dotFlux"), (float)fmax(flux, DOT_MIN_FLUX));
		glUniform1f(glGetUniformLocation(shader, "dotSigma"), (float)DOT_SIGMA_PX);
	}

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* 1 if body id is drawn by its own passes this frame. */
static int drawnInFull(int id, int fullBody, int moonFull)
{
	return id == fullBody || (id == BODY_MOON && moonFull);
}

void Planets_Draw(mat4 proj, mat4 view, dvec3 cam, int fullBody, int moonFull, int ringBody)
{
	glUseProgram(shader);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	int viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glUniform2f(glGetUniformLocation(shader, "viewportSize"), (float)viewport[2], (float)viewport[3]);

	glBindVertexArray(quadVAO);
	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GREATER);   /* reversed-Z */

	/* Discs: exact spheres with depth; edge coverage, glow and rings
	   blended. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_TRUE);
	for (int i = 0; i < BODY_COUNT; i++)
		if (Bodies_Get(i)->kind != BODY_KIND_STAR && !drawnInFull(i, fullBody, moonFull))
			drawBody(i, view, cam, 0, ringBody);

	/* Dots: additive light, depth-tested only. */
	glBlendFunc(GL_ONE, GL_ONE);
	glDepthMask(GL_FALSE);
	for (int i = 0; i < BODY_COUNT; i++)
		if (Bodies_Get(i)->kind != BODY_KIND_STAR && !drawnInFull(i, fullBody, moonFull))
			drawBody(i, view, cam, 1, ringBody);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glBindVertexArray(0);
}
