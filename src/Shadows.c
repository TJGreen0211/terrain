#include "engine/platform/Window.h"
#include "Shadows.h"
#include "WorldConfig.h"
#include <math.h>
#include <stdio.h>

ShadowCascades g_cascades;
int g_shadowDebug = 0;

static GLuint cascadeTex, cascadeFBO;

void Shadows_InitGL(void)
{
	/* Same format and sampling as the global map (Main.c initDepthbuffer):
	   float depth, LINEAR + compare mode = hardware 2x2 PCF per tap, border
	   at the far plane (lit). */
	glGenTextures(1, &cascadeTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, cascadeTex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, SHADOW_CASCADE_SIZE, SHADOW_CASCADE_SIZE,
	             SHADOW_CASCADES, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	glGenFramebuffers(1, &cascadeFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, cascadeFBO);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, cascadeTex, 0, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[shadows] cascade framebuffer incomplete\n");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static double dot3(dvec3 a, dvec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static dvec3 add3(dvec3 a, dvec3 b) { dvec3 r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
static dvec3 mul3(dvec3 a, double s) { dvec3 r = { a.x*s, a.y*s, a.z*s }; return r; }
static dvec3 cross3(dvec3 a, dvec3 b)
{
	dvec3 r = { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
	return r;
}
static dvec3 norm3(dvec3 a) { return mul3(a, 1.0 / sqrt(dot3(a, a))); }

/* Light basis for a sun direction: f = the way the light travels, r and u
   across it (Main.c getLightViewMatrix's convention). */
static void lightBasis(dvec3 sunDir, dvec3 *r, dvec3 *u, dvec3 *f)
{
	*f = mul3(sunDir, -1.0);
	dvec3 up = { 0.0, 1.0, 0.0 };
	if (fabs(f->y) > 0.99) { up.x = 1.0; up.y = 0.0; }
	*r = norm3(cross3(*f, up));
	*u = cross3(*r, *f);
}

/* Bounding sphere of the view-frustum slice between view distances n and
   f (t2 = tan^2 of the corner ray's angle off the axis): the centre sits
   on the axis where the near and far corners are equally far, or at the
   far plane when that is closer. Returns the centre's distance along the
   axis; *radius gets the radius. */
static double sliceSphere(double n, double f, double t2, double *radius)
{
	double zc = 0.5 * (n + f) * (1.0 + t2);
	if (zc > f)
		zc = f;
	*radius = sqrt((f - zc) * (f - zc) + f * f * t2);
	return zc;
}

void Shadows_Fit(dvec3 camWorld, dvec3 forward, dvec3 sunDir, double nearest, double farthest)
{
	g_cascades.count = 0;
	int n = SHADOW_CASCADES < SHADOW_MAX_CASCADES ? SHADOW_CASCADES : SHADOW_MAX_CASCADES;
	nearest = fmax(nearest, SHADOW_CASCADE_MIN_DIST_M);
	farthest = fmin(farthest, SHADOW_CASCADE_MAX_DIST_M);
	/* Too little range left to beat the global map. */
	if (n <= 0 || farthest < 2.0 * nearest)
		return;

	double tanY = tan(0.5 * CAMERA_FOV_Y_DEG * M_PI / 180.0);
	double aspect = getWindowWidth() / fmax(getWindowHeight(), 1.0f);
	double t2 = tanY * tanY * (1.0 + aspect * aspect);
	dvec3 r, u, f;
	lightBasis(sunDir, &r, &u, &f);
	forward = norm3(forward);

	/* Logarithmic splits: every cascade covers the same ratio of
	   distances, so texels grow with distance like pixels do. The first
	   slice starts at the camera (the near part is cheap to include). */
	double ratio = farthest / nearest;
	double sliceNear = 0.0;
	for (int i = 0; i < n; i++) {
		double sliceFar = nearest * pow(ratio, (double)(i + 1) / n);
		double radius;
		double zc = sliceSphere(sliceNear, sliceFar, t2, &radius);
		/* Quarter-octave radius steps: the size (and texel) only changes
		   in steps as the slice grows, not every frame. */
		radius = pow(2.0, ceil(4.0 * log2(radius)) / 4.0);
		double texel = 2.0 * radius / SHADOW_CASCADE_SIZE;

		/* Snap the centre to the texel grid in world coordinates (across
		   the light), so the map does not crawl as the camera moves. */
		dvec3 c = mul3(forward, zc);
		dvec3 w = add3(c, camWorld);
		double sx = dot3(w, r), sy = dot3(w, u);
		double qx = floor(sx / texel + 0.5) * texel, qy = floor(sy / texel + 0.5) * texel;
		c = add3(c, add3(mul3(r, qx - sx), mul3(u, qy - sy)));

		/* Box: +-radius across, depth from just up-sun of the sphere to its
		   far side. Casters further up-sun are clamped onto the near plane
		   (Shadows_BeginCascade), so the depth range, and its precision,
		   only spans the receivers. */
		double up = 1.05 * radius;
		dvec3 eye = add3(c, mul3(sunDir, up));
		mat4 v = scale(1.0);
		v.m[0][0] = (float)r.x; v.m[0][1] = (float)r.y; v.m[0][2] = (float)r.z;
		v.m[1][0] = (float)u.x; v.m[1][1] = (float)u.y; v.m[1][2] = (float)u.z;
		v.m[2][0] = (float)-f.x; v.m[2][1] = (float)-f.y; v.m[2][2] = (float)-f.z;
		v.m[0][3] = (float)-dot3(r, eye);
		v.m[1][3] = (float)-dot3(u, eye);
		v.m[2][3] = (float)dot3(f, eye);
		mat4 p = ortho((float)-radius, (float)radius, (float)-radius, (float)radius,
		               0.0f, (float)(up + radius));

		ShadowCascade *sc = &g_cascades.c[i];
		sc->lightSpace = multiplymat4(p, v);
		sc->center = c;
		sc->radius = radius;
		sc->texel = texel;
		/* Overlap the previous slice by the shader's blend zone. */
		sliceNear = sliceFar * 0.8;
	}
	g_cascades.count = n;
}

void Shadows_BeginCascade(int i)
{
	glBindFramebuffer(GL_FRAMEBUFFER, cascadeFBO);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, cascadeTex, 0, i);
	glViewport(0, 0, SHADOW_CASCADE_SIZE, SHADOW_CASCADE_SIZE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_CLAMP);
}

void Shadows_EndCascades(void)
{
	glDisable(GL_DEPTH_CLAMP);
}

void Shadows_BindReceiver(GLuint shader, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D_ARRAY, cascadeTex);
	glUniform1i(glGetUniformLocation(shader, "cascadeMap"), unit);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(shader, "cascadeCount"), g_cascades.count);
	glUniform1i(glGetUniformLocation(shader, "shadowDebug"), g_shadowDebug);
	for (int i = 0; i < g_cascades.count; i++) {
		const ShadowCascade *sc = &g_cascades.c[i];
		char name[32];
		snprintf(name, sizeof name, "cascadeSpace[%d]", i);
		glUniformMatrix4fv(glGetUniformLocation(shader, name), 1, GL_FALSE, &sc->lightSpace.m[0][0]);
		snprintf(name, sizeof name, "cascadeSphere[%d]", i);
		glUniform4f(glGetUniformLocation(shader, name), (float)sc->center.x, (float)sc->center.y,
		            (float)sc->center.z, (float)sc->radius);
		snprintf(name, sizeof name, "cascadeTexel[%d]", i);
		glUniform1f(glGetUniformLocation(shader, name), (float)sc->texel);
	}
}
