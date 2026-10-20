#include "Main.h"
#include "Atmosphere.h"

static GLuint atmosphereVAO, atmosphereVBO;
static int atmosphereVertexCount;

/* OpenGLSolar Earth values. Shell 4.24% above the surface (2x the original
   2.12% so tall terrain stays inside). A macro so the static initializer
   below and atmosphereDefaults() share one copy. */
#define ATMOSPHERE_DEFAULTS { \
	.outerScale = 1.0424f, \
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
static void setScatteringUniforms(GLuint shader, float fInner, const atmosphereParams *atmo)
{
	glUniform1f(glGetUniformLocation(shader, "fInnerRadius"), fInner);
	glUniform1f(glGetUniformLocation(shader, "fOuterRadius"), fInner * atmo->outerScale);
	glUniform3f(glGetUniformLocation(shader, "C_R"), atmo->C_R.x, atmo->C_R.y, atmo->C_R.z);
	glUniform1f(glGetUniformLocation(shader, "E"), atmo->E);
	glUniform1f(glGetUniformLocation(shader, "K_R"), atmo->K_R);
	glUniform1f(glGetUniformLocation(shader, "K_M"), atmo->K_M);
	glUniform1f(glGetUniformLocation(shader, "G_M"), atmo->G_M);
}

void initAtmosphere(GLuint shader, sphere mesh)
{
	atmosphereVertexCount = mesh.vertexNumber;

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
	float fOuter = fInner * atmo->outerScale;
	if (fOuter <= fInner) {
		// no real shell; guard against a degenerate dome
		return;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	glUseProgram(shader);

	/* Floating origin: modelmat is the camera-relative planet centre and the
	   camera sits at the origin, so the camera in the shell-centred frame is
	   just -planetTransform. */
	vec3 camPosition = {-modelmat.m[0][3], -modelmat.m[1][3], -modelmat.m[2][3]};

	mat4 m = multiplymat4(modelmat, scale(fOuter));

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

	// The dome is an outward-facing sphere. When the camera is inside the
	// outer atmosphere every visible dome triangle is a back face, so
	// swap the cull face (the paper inverts the sky dome's front faces
	// for the camera-in-atmosphere case). The sky is a pure additive
	// pass and must not write depth, or its surface would hide the
	// moon/objects beyond the shell behind it.
	float camDistSq = camPosition.x*camPosition.x + camPosition.y*camPosition.y + camPosition.z*camPosition.z;
	int inside = camDistSq < fOuter*fOuter;
	glDepthMask(GL_FALSE);
	if (inside) {
		// The dome surface surrounds the camera: its far side lies
		// BEHIND the planet, so depth testing would punch a skyless
		// hole where the planet is and pop the fog on shell crossing.
		// The shader already clips the scattering ray at the planet
		// surface, so blending the in-scatter over the planet is the
		// correct aerial-perspective result.
		glCullFace(GL_FRONT);
		glDisable(GL_DEPTH_TEST);
	}
	glBindVertexArray(atmosphereVAO);
	glDrawArrays(GL_TRIANGLES, 0, atmosphereVertexCount);
	if (inside) glEnable(GL_DEPTH_TEST);
	glCullFace(GL_BACK);
	glBindVertexArray(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void setAtmosphereExtinction(GLuint shader, float fInner, const atmosphereParams *atmo)
{
	glUniform1f(glGetUniformLocation(shader, "hasAtmosphere"), atmo->outerScale > 1.0f ? 1.0f : 0.0f);
	setScatteringUniforms(shader, fInner, atmo);
}
