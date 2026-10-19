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
                        float fInner, float fOuter, vec3 C_R, float E, vec3 lightPosition)
{
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
	glUniform1f(glGetUniformLocation(shader, "time"), (float)glfwGetTime());

	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);

	glDepthMask(GL_FALSE);
	if (fOuter <= fInner) {
		// no real shell; guard against a degenerate dome
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		return;
	}
	// The dome is an outward-facing sphere. When the camera is inside the
	// outer atmosphere every visible dome triangle is a back face, so
	// swap the cull face (the paper inverts the sky dome's front faces
	// for the camera-in-atmosphere case). The sky is a pure additive
	// pass and must not write depth, or its surface would hide the
	// moon/objects beyond the shell behind it.
	float camDistSq = camPosition.x*camPosition.x + camPosition.y*camPosition.y + camPosition.z*camPosition.z;
	if (camDistSq < fOuter*fOuter) {
		glCullFace(GL_FRONT);
	}
	glBindVertexArray(atmosphereVAO);
	glDrawArrays(GL_TRIANGLES, 0, atmosphereVertexCount);
	glCullFace(GL_BACK);
	glBindVertexArray(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}
