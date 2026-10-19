#include "Main.h"
#include "Atmosphere.h"

static GLuint atmosphereVAO, atmosphereVBO;
static int atmosphereVertexCount;

void initAtmosphere(GLuint shader, sphere mesh)
{
	atmosphereVertexCount = mesh.vertexNumber;

    glGenVertexArrays(1, &atmosphereVAO);
	glBindVertexArray(atmosphereVAO);
	glGenBuffers(1, &atmosphereVBO);
	glBindBuffer(GL_ARRAY_BUFFER, atmosphereVBO);
	glBufferData(GL_ARRAY_BUFFER, mesh.size, mesh.points, GL_STATIC_DRAW);
	GLuint vPosition = glGetAttribLocation(shader, "vPosition");
    glEnableVertexAttribArray(vPosition);
    glVertexAttribPointer(vPosition, 3, GL_FLOAT, GL_FALSE, 3*sizeof(GLfloat), BUFFER_OFFSET(0));
	glBindVertexArray(0);
}

void setAtmosphereSurfaceUniforms(GLuint shader, float fInner, float fOuter, vec3 C_R)
{
	glUseProgram(shader);
	glUniform1f(glGetUniformLocation(shader, "fInnerRadius"), fInner);
	glUniform1f(glGetUniformLocation(shader, "fOuterRadius"), fOuter);
	glUniform3f(glGetUniformLocation(shader, "C_R"), C_R.x, C_R.y, C_R.z);
}

void drawAtmosphereDome(GLuint shader, mat4 proj, mat4 view, mat4 modelmat,
                        float fInner, float fOuter, vec3 C_R, float E, vec3 lightPosition)
{
	if (fOuter <= fInner) {
		return;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	glUseProgram(shader);

	mat4 tv = transposemat4(multiplymat4(modelmat, getViewPosition()));
	vec4 camMult = {-tv.m[3][0], -tv.m[3][1], -tv.m[3][2], -tv.m[3][3]};
	vec4 camPosition = multiplymat4vec4(tv, camMult);

	mat4 m = multiplymat4(modelmat, scale(fOuter));

	// Per-body solar brightness + Rayleigh tint (Earth values from OpenGLSolar).
	// The white Mie term is summed on top, so C_R's RGB sets each limb's hue.
	glUniform1f(glGetUniformLocation(shader, "fInnerRadius"), fInner);
	glUniform1f(glGetUniformLocation(shader, "fOuterRadius"), fOuter);
	glUniform3f(glGetUniformLocation(shader, "camPosition"), camPosition.x, camPosition.y, camPosition.z);
	glUniform3f(glGetUniformLocation(shader, "C_R"), C_R.x, C_R.y, C_R.z);
	glUniform1f(glGetUniformLocation(shader, "E"), E);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), lightPosition.x, lightPosition.y, lightPosition.z);
	glUniform3f(glGetUniformLocation(shader, "shellCenter"),
	            modelmat.m[0][3], modelmat.m[1][3], modelmat.m[2][3]);

	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);

	glDepthMask(GL_FALSE);
	// The dome is an outward-facing sphere. When the camera is inside the
	// outer atmosphere every visible dome triangle is a back face, so
	// swap the cull face (the paper inverts the sky dome's front faces
	// for the camera-in-atmosphere case). The sky is a pure additive
	// pass and must not write depth, or its surface would hide the
	// moon/objects beyond the shell behind it.
	float camDistSq = camPosition.x*camPosition.x + camPosition.y*camPosition.y + camPosition.z*camPosition.z;
	int inside = camDistSq < fOuter*fOuter;
	GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
	if (inside) {
		glCullFace(GL_FRONT);
		// The far hemisphere lies behind the planet, but its scattering
		// still belongs over the surface along this view ray.
		glDisable(GL_DEPTH_TEST);
	}
	glBindVertexArray(atmosphereVAO);
	glDrawArrays(GL_TRIANGLES, 0, atmosphereVertexCount);
	if (inside && depthTestEnabled) {
		glEnable(GL_DEPTH_TEST);
	}
	glCullFace(GL_BACK);
	glBindVertexArray(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}
