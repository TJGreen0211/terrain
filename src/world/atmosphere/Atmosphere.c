#include "engine/gfx/Buffers.h"
#include "world/atmosphere/Atmosphere.h"
#include "world/WorldConfig.h"

static GLuint atmosphereVAO, atmosphereVBO;
static int atmosphereVertexCount;
/* The dome mesh's scale over the shell radius: its flat triangles sit inside
   the sphere they approximate, so drawn at the shell radius the polygonal
   silhouette cut off the outermost (grazing) rays in straight segments.
   1 / (closest triangle plane to the centre) makes the mesh enclose the
   whole shell; the shader discards rays that miss it. */
static float domeScale = 1.0f;

/* Scene depth for the dome's surface clip (Atmosphere_SetSceneDepth). Unit
   12: 0-11 are taken (terrain detail uses 11). */
#define ATMOSPHERE_DEPTH_UNIT 12
static GLuint sceneDepthTex;
static int sceneDepthSamples;
static float sceneNear;

void Atmosphere_SetSceneDepth(GLuint depthTex, int samples, float nearPlane)
{
	sceneDepthTex = depthTex;
	sceneDepthSamples = samples;
	sceneNear = nearPlane;
}

/* OpenGLSolar Earth values; shell and scale height from WorldConfig.h. A
   macro so the static initializer below and atmosphereDefaults() share
   one copy. */
#define ATMOSPHERE_DEFAULTS { \
	.shellHeight = (float)ATMOSPHERE_SHELL_HEIGHT_M, \
	.scaleHeight = (float)ATMOSPHERE_SCALE_HEIGHT_M, \
	.C_R = { 0.3f, 0.7f, 1.0f }, \
	.E = 14.3f, \
	.K_R = 0.166f, \
	.K_M = 0.0025f, \
	.G_M = -0.85f, \
}

atmosphereParams g_atmosphere = ATMOSPHERE_DEFAULTS;

atmosphereParams atmosphereDefaults(void)
{
	atmosphereParams p = ATMOSPHERE_DEFAULTS;
	return p;
}

/* The scattering uniforms every atmosphere-aware program shares. Programs
   that don't declare one (quad.frag has no E or G_M) get location -1, which
   glUniform ignores. */
/* atmodensity.glsl's density, for the CPU-side zenith optical depth. */
static double atmoDensity(double h, double shell, double scaleH)
{
	double top = exp(-shell / scaleH);
	return fmax(exp(-fmax(h, 0.0) / scaleH) - top, 0.0) / (1.0 - top);
}

/* quad.frag / water.frag's atmoOptic straight up through the shell: the same
   5-sample midpoint march, in shell-height units. */
static double zenithOptic(const atmosphereParams *atmo)
{
	double shell = atmo->shellHeight, sum = 0.0;
	if (shell <= 0.0 || atmo->scaleHeight <= 0.0)
		return 0.0;
	for (int i = 0; i < 5; i++)
		sum += atmoDensity((i + 0.5) * shell / 5.0, shell, atmo->scaleHeight);
	return sum / 5.0;
}

static void setScatteringUniforms(GLuint shader, float fInner, const atmosphereParams *atmo)
{
	glUniform1f(glGetUniformLocation(shader, "atmoTauZenith"), (float)zenithOptic(atmo));
	glUniform1f(glGetUniformLocation(shader, "fInnerRadius"), fInner);
	glUniform1f(glGetUniformLocation(shader, "fOuterRadius"), fInner + atmo->shellHeight);
	glUniform1f(glGetUniformLocation(shader, "fScaleHeight"), atmo->scaleHeight);
	glUniform3f(glGetUniformLocation(shader, "C_R"), atmo->C_R.x, atmo->C_R.y, atmo->C_R.z);
	glUniform1f(glGetUniformLocation(shader, "E"), atmo->E);
	glUniform1f(glGetUniformLocation(shader, "K_R"), atmo->K_R);
	glUniform1f(glGetUniformLocation(shader, "K_M"), atmo->K_M);
	glUniform1f(glGetUniformLocation(shader, "G_M"), atmo->G_M);
}

void initAtmosphere(GLuint shader, sphere mesh)
{
	atmosphereVertexCount = mesh.vertexNumber;

	double minPlane = 1.0;
	for (int t = 0; t + 2 < mesh.vertexNumber; t += 3) {
		vec3 a = mesh.points[t], b = mesh.points[t + 1], c = mesh.points[t + 2];
		double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
		double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
		double nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
		double len = sqrt(nx*nx + ny*ny + nz*nz);
		if (len > 0.0)
			minPlane = fmin(minPlane, fabs(nx*a.x + ny*a.y + nz*a.z) / len);
	}
	domeScale = (float)(1.0 / fmax(minPlane, 0.5)) * 1.0001f;

    glGenVertexArrays(1, &atmosphereVAO);
	glBindVertexArray(atmosphereVAO);
	glGenBuffers(1, &atmosphereVBO);
	glBindBuffer(GL_ARRAY_BUFFER, atmosphereVBO);
	glBufferData(GL_ARRAY_BUFFER, mesh.size + mesh.nsize, NULL, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, mesh.size, mesh.points);
	glBufferSubData(GL_ARRAY_BUFFER, mesh.size, mesh.nsize, mesh.normals);
	GLuint vPosition = glGetAttribLocation(shader, "vPosition");
    glEnableVertexAttribArray(vPosition);
    glVertexAttribPointer(vPosition, 3, GL_FLOAT, GL_FALSE, 3*sizeof(GLfloat), BUFFER_OFFSET(0));

    GLuint vNormal = glGetAttribLocation(shader, "vNormal");
    glEnableVertexAttribArray(vNormal);
    glVertexAttribPointer(vNormal, 3, GL_FLOAT, GL_FALSE, 3*sizeof(GLfloat), BUFFER_OFFSET(mesh.size));
	glBindVertexArray(0);
}

void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, const atmosphereParams *atmo, vec4 lightPosition)
{
	float fOuter = fInner + atmo->shellHeight;
	if (fOuter <= fInner) {
		// no real shell; guard against a degenerate dome
		return;
	}

	/* Additive, times (1 - destination alpha): the cloud slab leaves its
	   cover there and has already added the air in front of itself
	   (clouds.frag); elsewhere the alpha is 0 (Main.c clears it). */
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);

	glUseProgram(shader);

	/* Floating origin: modelmat is the camera-relative planet centre and the
	   camera sits at the origin, so the camera in the shell-centred frame is
	   just -planetTransform. */
	vec3 camPosition = {-modelmat.m[0][3], -modelmat.m[1][3], -modelmat.m[2][3]};

	mat4 m = multiplymat4(modelmat, scale(fOuter * domeScale));

	// Per-body solar brightness + Rayleigh tint (Earth values from OpenGLSolar).
	// The white Mie term is summed on top, so C_R's RGB sets each limb's hue.
	setScatteringUniforms(shader, fInner, atmo);
	glUniform3f(glGetUniformLocation(shader, "camPosition"), camPosition.x, camPosition.y, camPosition.z);
	glUniform4f(glGetUniformLocation(shader, "lightPos"), lightPosition.x, lightPosition.y, lightPosition.z, lightPosition.w);
	// Camera-relative body centre: modelmat is a pure translation, its
	// translation column is the shell sphere origin (row-vector
	// convention, m[0][3]..m[2][3])
	glUniform3f(glGetUniformLocation(shader, "shellCenter"), modelmat.m[0][3], modelmat.m[1][3], modelmat.m[2][3]);

	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);

	/* Scene depth, to end each ray at the visible surface: view z = near /
	   depth, along the camera's forward axis (minus the view matrix's
	   third row; camera-relative world = the dome's local frame). */
	glActiveTexture(GL_TEXTURE0 + ATMOSPHERE_DEPTH_UNIT);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneDepthTex);
	glUniform1i(glGetUniformLocation(shader, "sceneDepth"), ATMOSPHERE_DEPTH_UNIT);
	glUniform1i(glGetUniformLocation(shader, "sceneSamples"), sceneDepthTex ? sceneDepthSamples : 0);
	glUniform1f(glGetUniformLocation(shader, "depthNear"), sceneNear);
	glUniform3f(glGetUniformLocation(shader, "camForward"), -view.m[2][0], -view.m[2][1], -view.m[2][2]);
	glActiveTexture(GL_TEXTURE0);

	// The dome is an outward-facing sphere. When the camera is inside the
	// outer atmosphere every visible dome triangle is a back face, so
	// swap the cull face (the paper inverts the sky dome's front faces
	// for the camera-in-atmosphere case). No depth test or write either
	// way: the shader ends each ray at the scene depth itself (anything in
	// front of the air discards), the in-scatter is blended over the planet
	// as aerial perspective, and the depth texture it reads is the bound
	// target's own attachment.
	float camDistSq = camPosition.x*camPosition.x + camPosition.y*camPosition.y + camPosition.z*camPosition.z;
	/* Inside the drawn mesh (not just the shell): only back faces show. */
	float meshR = fOuter * domeScale;
	int inside = camDistSq < meshR*meshR;
	glDepthMask(GL_FALSE);
	glDisable(GL_DEPTH_TEST);
	if (inside)
		glCullFace(GL_FRONT);
	glBindVertexArray(atmosphereVAO);
	glDrawArrays(GL_TRIANGLES, 0, atmosphereVertexCount);
	glEnable(GL_DEPTH_TEST);
	glCullFace(GL_BACK);
	glBindVertexArray(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void setAtmosphereExtinction(GLuint shader, float fInner, const atmosphereParams *atmo)
{
	glUniform1f(glGetUniformLocation(shader, "hasAtmosphere"), atmo->shellHeight > 0.0f ? 1.0f : 0.0f);
	setScatteringUniforms(shader, fInner, atmo);
}
