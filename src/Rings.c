#include "Main.h"
#include "Rings.h"
#include <math.h>
#include <stdlib.h>

/* Ring geometry in planet radii. Saturn's main rings span ~1.24-2.27. */
#define RING_INNER 1.30f
#define RING_OUTER 2.30f
/* Ring-plane tilt (degrees): normal = rotX(TILT_X) * rotZ(TILT_Z) * +Y.
   The sun sits in the planet's XZ plane, so an untilted ring would be
   edge-on to it (unlit, no shadow). These put the sun ~18 deg above the
   plane, on the same side as the spawn view. */
#define RING_TILT_Z 35.0
#define RING_TILT_X 10.0
/* Sun angular radius (radians): sets the penumbra width of the planet's
   shadow on the ring and of the ring's shadow on everything else. */
#define RING_SUN_ANGLE 0.012f
#define RING_PROFILE_SIZE 4096
/* Texture unit for ringProfile in every program; setBodyUniforms uses 0-8. */
#define RING_PROFILE_UNIT 9

/* Close-up field. The sheet fades out over RING_FADE_* (distance from the
   camera, as fractions of ROCK_RANGE) where the rocks and dust take over. */
#define ROCK_RANGE 70.0f
#define ROCK_CELL 3.5f
#define ROCK_DENSITY 0.35f          /* spawn probability per unit optical depth, per mesh */
#define ROCK_MIN_SIZE 0.12f
#define ROCK_MAX_SIZE 2.5f
#define ROCK_MESH_SCALE (1.0f/1.6f) /* rock*.obj extents (~1.6) -> unit radius */
#define RING_HALF_THICKNESS 3.0f
#define RING_FADE_NEAR 0.35f
#define RING_FADE_FAR 0.9f
#define DUST_RANGE 30.0f
#define DUST_CELL 1.0f
#define DUST_PER_CELL 3
#define DUST_DENSITY 0.6f
#define DUST_HALF_THICKNESS 2.0f

static struct {
	GLuint profileTex;
	GLuint sheetShader, rockShader, dustShader;
	GLuint emptyVAO;
	GLuint rockVAO[3];
	int rockVerts[3];
	unsigned int seed;

	/* Ring frame (world, constant): normal + in-plane basis. */
	dvec3 n, u, v;

	/* Per frame (Rings_SetFrame). */
	vec3 center;          /* camera-relative planet centre */
	vec3 light;           /* camera-relative light */
	double camLocalX, camLocalY, camHeight;
	float inner, outer, planetRadius;
} g_ring;

/* ---- Profile bake ------------------------------------------------------ */

static unsigned int pcgHash(unsigned int v)
{
	unsigned int s = v * 747796405u + 2891336453u;
	unsigned int w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}

static float rnd(unsigned int *state)
{
	*state = pcgHash(*state);
	return (float)(*state / 4294967296.0);
}

static float hashf(unsigned int seed, int i)
{
	return (float)(pcgHash(seed ^ pcgHash((unsigned int)i)) / 4294967296.0);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float smoothstepf(float e0, float e1, float x)
{
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

static float noise1(unsigned int seed, float x)
{
	float fl = floorf(x);
	int i = (int)fl;
	float f = x - fl;
	f = f * f * (3.0f - 2.0f * f);
	return lerpf(hashf(seed, i), hashf(seed, i + 1), f);
}

/* 1D fbm in [0,1]. */
static float fbm1(unsigned int seed, float x, int octaves)
{
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; o++) {
		sum += amp * noise1(seed + (unsigned int)o * 1013u, x);
		norm += amp;
		amp *= 0.5f;
		x *= 2.13f;
	}
	return sum / norm;
}

/* 1 inside [a,b] with soft edges. */
static float band(float r, float a, float b, float soft)
{
	return smoothstepf(a - soft, a + soft, r) * (1.0f - smoothstepf(b - soft, b + soft, r));
}

/* 0 at the gap centre, 1 outside its width. */
static float gap(float r, float c, float w)
{
	return smoothstepf(0.5f * w, w, fabsf(r - c));
}

#define RING_GAPS 10

/* Saturn-like layout with seeded proportions over r in [0,1] (inner ->
   outer edge): faint C ring, dense B ring, Cassini division, A ring with an
   Encke-style gap, narrow F ring. Medium + fine fbm ringlets and a set of
   narrow gaps on top. Colour runs from dark dust (thin) to bright ice
   (dense), with a seeded tint. */
static void bakeProfile(unsigned int seed)
{
	unsigned int st = seed * 2654435761u + 0x51ED27u;
	float bStart = 0.20f + 0.10f * rnd(&st);
	float bEnd = 0.58f + 0.08f * rnd(&st);
	float cassW = 0.035f + 0.04f * rnd(&st);
	float aStart = bEnd + cassW;
	float aEnd = 0.92f + 0.04f * rnd(&st);
	float fPos = fminf(aEnd + 0.015f + 0.02f * rnd(&st), 0.99f);
	float encke = aStart + (aEnd - aStart) * (0.72f + 0.18f * rnd(&st));
	float enckeW = 0.003f + 0.004f * rnd(&st);
	float tauC = 0.06f + 0.10f * rnd(&st);
	float tauB = 1.2f + 1.3f * rnd(&st);
	float tauA = 0.4f + 0.4f * rnd(&st);
	float gapPos[RING_GAPS], gapW[RING_GAPS];
	for (int g = 0; g < RING_GAPS; g++) {
		gapPos[g] = 0.03f + (aEnd - 0.03f) * rnd(&st);
		gapW[g] = 0.001f + 0.003f * rnd(&st) * rnd(&st);
	}
	/* Dense-ice colour: dusty tan through to blue-grey. */
	float tint = rnd(&st);
	float dust[3] = { 0.28f, 0.25f, 0.22f };
	float ice[3] = { lerpf(0.66f, 0.60f, tint), lerpf(0.58f, 0.63f, tint), lerpf(0.46f, 0.66f, tint) };
	unsigned int ns = pcgHash(seed ^ 0xA5A5A5A5u);

	float *texels = malloc(RING_PROFILE_SIZE * 4 * sizeof(float));
	for (int i = 0; i < RING_PROFILE_SIZE; i++) {
		float r = (i + 0.5f) / RING_PROFILE_SIZE;

		float tau = tauC * (0.6f + 0.4f * r / bStart) * band(r, 0.0f, bStart, 0.004f)
		          * smoothstepf(0.0f, 0.04f, r);
		/* B ring: ramps up from its inner edge, sharp outer edge. */
		tau += tauB * (0.65f + 0.7f * fbm1(ns + 1u, r * 22.0f, 3))
		     * band(r, bStart, bEnd, 0.002f) * smoothstepf(bStart - 0.002f, bStart + 0.03f, r);
		/* Cassini division: faint ringlets only. */
		tau += 0.05f * (0.5f + fbm1(ns + 2u, r * 300.0f, 2)) * band(r, bEnd, aStart, 0.002f);
		/* A ring: thins outward. */
		tau += tauA * (1.0f - 0.35f * (r - aStart) / (aEnd - aStart))
		     * (0.85f + 0.3f * fbm1(ns + 3u, r * 40.0f, 3)) * band(r, aStart, aEnd, 0.0015f);
		/* F ring. */
		float df = (r - fPos) / 0.0015f;
		tau += 0.5f * expf(-df * df);

		tau *= 0.45f + 1.1f * fbm1(ns + 4u, r * 160.0f, 4);
		tau *= 0.75f + 0.5f * fbm1(ns + 5u, r * 1100.0f, 3);
		tau *= gap(r, encke, enckeW);
		for (int g = 0; g < RING_GAPS; g++)
			tau *= 1.0f - 0.85f * (1.0f - gap(r, gapPos[g], gapW[g]));
		tau = fmaxf(tau, 0.0f);

		float dense = smoothstepf(0.03f, 1.6f, tau);
		float bright = 0.85f + 0.3f * fbm1(ns + 6u, r * 14.0f, 3);
		float hue = (fbm1(ns + 7u, r * 35.0f, 2) - 0.5f) * 0.3f;
		float col[3];
		for (int c = 0; c < 3; c++)
			col[c] = lerpf(dust[c], ice[c], dense) * bright;
		col[0] *= 1.0f + hue;
		col[2] *= 1.0f - hue;
		/* A ring a little greyer than B. */
		float aW = 0.3f * band(r, aStart, aEnd, 0.01f);
		float lum = 0.3f * col[0] + 0.59f * col[1] + 0.11f * col[2];
		for (int c = 0; c < 3; c++) {
			col[c] = lerpf(col[c], lum, aW);
			texels[i * 4 + c] = fminf(fmaxf(col[c], 0.02f), 0.95f);
		}
		texels[i * 4 + 3] = tau;
	}
	/* Clamp-to-edge reads the end texels outside [0,1]: keep them empty. */
	texels[3] = 0.0f;
	texels[(RING_PROFILE_SIZE - 1) * 4 + 3] = 0.0f;

	glBindTexture(GL_TEXTURE_1D, g_ring.profileTex);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F, RING_PROFILE_SIZE, 0, GL_RGBA, GL_FLOAT, texels);
	glGenerateMipmap(GL_TEXTURE_1D);
	glBindTexture(GL_TEXTURE_1D, 0);
	free(texels);
}

/* ---- Setup ------------------------------------------------------------- */

static dvec3 dnormalize(dvec3 a)
{
	return dscalarvec3(a, 1.0 / dlengthvec3(a));
}

static dvec3 dcross(dvec3 a, dvec3 b)
{
	dvec3 c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	return c;
}

static double ddot(dvec3 a, dvec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

void Rings_Init(unsigned int seed, const GLuint rockVAO[3], const int rockVerts[3])
{
	createShader(&g_ring.sheetShader, "shaders/ring.vert", "shaders/ring.frag");
	createShader(&g_ring.rockShader, "shaders/rock.vert", "shaders/rock.frag");
	createShader(&g_ring.dustShader, "shaders/ringdust.vert", "shaders/ringdust.frag");
	/* The sheet and dust are generated from gl_VertexID. */
	glGenVertexArrays(1, &g_ring.emptyVAO);
	for (int i = 0; i < 3; i++) {
		g_ring.rockVAO[i] = rockVAO[i];
		g_ring.rockVerts[i] = rockVerts[i];
	}

	double tz = RING_TILT_Z * M_PI / 180.0, tx = RING_TILT_X * M_PI / 180.0;
	dvec3 n = { -sin(tz), cos(tz) * cos(tx), cos(tz) * sin(tx) };
	dvec3 ref = { 0.0, 0.0, 1.0 };
	g_ring.n = dnormalize(n);
	g_ring.u = dnormalize(dcross(g_ring.n, ref));
	g_ring.v = dcross(g_ring.n, g_ring.u);

	glGenTextures(1, &g_ring.profileTex);
	glBindTexture(GL_TEXTURE_1D, g_ring.profileTex);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_1D, 0);
	Rings_Regenerate(seed);

	GLuint own[3] = { g_ring.sheetShader, g_ring.rockShader, g_ring.dustShader };
	for (int i = 0; i < 3; i++) {
		glUseProgram(own[i]);
		glUniform1i(glGetUniformLocation(own[i], "ringProfile"), RING_PROFILE_UNIT);
	}
}

void Rings_Regenerate(unsigned int seed)
{
	g_ring.seed = pcgHash(seed);
	bakeProfile(seed);
}

void Rings_AttachReceiver(GLuint shader)
{
	glUseProgram(shader);
	glUniform1i(glGetUniformLocation(shader, "ringProfile"), RING_PROFILE_UNIT);
	glUniform1f(glGetUniformLocation(shader, "hasRing"), 0.0f);
}

void Rings_SetFrame(dvec3 planetWorld, dvec3 camWorld, float planetRadius, vec4 lightPosition)
{
	dvec3 rel = dsubvec3(camWorld, planetWorld);
	g_ring.camLocalX = ddot(rel, g_ring.u);
	g_ring.camLocalY = ddot(rel, g_ring.v);
	g_ring.camHeight = ddot(rel, g_ring.n);
	g_ring.center = dvec3tovec3(dscalarvec3(rel, -1.0));
	g_ring.light.x = lightPosition.x;
	g_ring.light.y = lightPosition.y;
	g_ring.light.z = lightPosition.z;
	g_ring.planetRadius = planetRadius;
	g_ring.inner = planetRadius * RING_INNER;
	g_ring.outer = planetRadius * RING_OUTER;
}

static void bindProfile(void)
{
	glActiveTexture(GL_TEXTURE0 + RING_PROFILE_UNIT);
	glBindTexture(GL_TEXTURE_1D, g_ring.profileTex);
	glActiveTexture(GL_TEXTURE0);
}

static void setVec3(GLuint shader, const char *name, vec3 a)
{
	glUniform3f(glGetUniformLocation(shader, name), a.x, a.y, a.z);
}

static void setDVec3(GLuint shader, const char *name, dvec3 a)
{
	glUniform3f(glGetUniformLocation(shader, name), (float)a.x, (float)a.y, (float)a.z);
}

/* Uniforms shared by the receivers and the ring's own passes. */
static void setRingShape(GLuint shader)
{
	setVec3(shader, "ringCenter", g_ring.center);
	setDVec3(shader, "ringNormal", g_ring.n);
	glUniform2f(glGetUniformLocation(shader, "ringRadii"), g_ring.inner, g_ring.outer);
	glUniform1f(glGetUniformLocation(shader, "ringSunAngle"), RING_SUN_ANGLE);
	glUniform2f(glGetUniformLocation(shader, "ringFade"), ROCK_RANGE * RING_FADE_NEAR, ROCK_RANGE * RING_FADE_FAR);
}

void Rings_BindReceiver(GLuint shader)
{
	glUseProgram(shader);
	glUniform1f(glGetUniformLocation(shader, "hasRing"), 1.0f);
	setRingShape(shader);
	bindProfile();
}

/* Program setup for the ring's own passes (sheet, rocks, dust). */
static void useRingProgram(GLuint shader, mat4 proj, mat4 view)
{
	glUseProgram(shader);
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &view.m[0][0]);
	setRingShape(shader);
	setDVec3(shader, "ringU", g_ring.u);
	setDVec3(shader, "ringV", g_ring.v);
	setVec3(shader, "lightPosition", g_ring.light);
	glUniform1f(glGetUniformLocation(shader, "planetRadius"), g_ring.planetRadius);
	glUniform1f(glGetUniformLocation(shader, "time"), (float)glfwGetTime());
	bindProfile();
}

/* Hashed spawn grid: a gridN x gridN window of ring-plane cells around the
   camera. The base cell is floored in double so the window never jitters,
   and the camera's plane offset is passed separately so the shaders only
   ever subtract nearby coordinates. Returns 0 when the camera is too far
   from the ring for the field to be visible. */
static int setSpawnGrid(GLuint shader, float cell, float range, float halfThickness, int *gridN)
{
	double rc = sqrt(g_ring.camLocalX * g_ring.camLocalX + g_ring.camLocalY * g_ring.camLocalY);
	if (fabs(g_ring.camHeight) > range + halfThickness || rc < g_ring.inner - range || rc > g_ring.outer + range)
		return 0;
	int half = (int)ceilf(range / cell);
	*gridN = 2 * half + 1;
	glUniform2i(glGetUniformLocation(shader, "baseCell"),
	            (int)floor(g_ring.camLocalX / cell), (int)floor(g_ring.camLocalY / cell));
	glUniform2f(glGetUniformLocation(shader, "camLocal"), (float)g_ring.camLocalX, (float)g_ring.camLocalY);
	glUniform1f(glGetUniformLocation(shader, "camHeight"), (float)g_ring.camHeight);
	glUniform1i(glGetUniformLocation(shader, "gridN"), *gridN);
	glUniform1f(glGetUniformLocation(shader, "cellSize"), cell);
	glUniform1f(glGetUniformLocation(shader, "range"), range);
	glUniform1f(glGetUniformLocation(shader, "halfThickness"), halfThickness);
	return 1;
}

/* ---- Draws ------------------------------------------------------------- */

void Rings_DrawRocks(mat4 proj, mat4 view)
{
	GLuint s = g_ring.rockShader;
	int gridN;
	useRingProgram(s, proj, view);
	if (!setSpawnGrid(s, ROCK_CELL, ROCK_RANGE, RING_HALF_THICKNESS, &gridN))
		return;
	glUniform1f(glGetUniformLocation(s, "density"), ROCK_DENSITY);
	glUniform2f(glGetUniformLocation(s, "sizeRange"), ROCK_MIN_SIZE, ROCK_MAX_SIZE);
	glUniform1f(glGetUniformLocation(s, "meshScale"), ROCK_MESH_SCALE);
	/* rock.frag faces the normal toward the viewer, so the meshes' winding
	   does not matter. */
	glDisable(GL_CULL_FACE);
	for (int i = 0; i < 3; i++) {
		glUniform1ui(glGetUniformLocation(s, "seed"), g_ring.seed ^ pcgHash(0x9E37u + (unsigned int)i));
		glBindVertexArray(g_ring.rockVAO[i]);
		glDrawArraysInstanced(GL_TRIANGLES, 0, g_ring.rockVerts[i], gridN * gridN);
	}
	glBindVertexArray(0);
	glEnable(GL_CULL_FACE);
}

/* Premultiplied-alpha passes that test against the scene depth but do not
   write it. */
static void beginTranslucent(void)
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
}

static void endTranslucent(void)
{
	glEnable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void Rings_DrawSheet(mat4 proj, mat4 view)
{
	useRingProgram(g_ring.sheetShader, proj, view);
	beginTranslucent();
	glBindVertexArray(g_ring.emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
	endTranslucent();
}

void Rings_DrawDust(mat4 proj, mat4 view)
{
	GLuint s = g_ring.dustShader;
	int gridN;
	useRingProgram(s, proj, view);
	if (!setSpawnGrid(s, DUST_CELL, DUST_RANGE, DUST_HALF_THICKNESS, &gridN))
		return;
	glUniform1ui(glGetUniformLocation(s, "seed"), g_ring.seed ^ 0xD057u);
	glUniform1i(glGetUniformLocation(s, "perCell"), DUST_PER_CELL);
	glUniform1f(glGetUniformLocation(s, "density"), DUST_DENSITY);
	/* Pixels per world unit at unit distance (45 deg vertical fov). */
	glUniform1f(glGetUniformLocation(s, "projScale"), getWindowHeight() / (2.0f * tanf(22.5f * (float)M_PI / 180.0f)));
	beginTranslucent();
	glEnable(GL_PROGRAM_POINT_SIZE);
	glBindVertexArray(g_ring.emptyVAO);
	glDrawArrays(GL_POINTS, 0, gridN * gridN * DUST_PER_CELL);
	glBindVertexArray(0);
	glDisable(GL_PROGRAM_POINT_SIZE);
	endTranslucent();
}
