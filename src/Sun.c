#include "Common.h"
#include "CommonGL.h"
#include "Sun.h"

static GLuint sunTexture;
static GLuint sunShader;
static GLuint sunQuadVAO, sunQuadVBO;

void initSun(void)
{
	sunTexture = loadTexture("../assets/textures/planets/sun2.jpg", 0);

	createShader(&sunShader, "shaders/sun.vert", "shaders/sun.frag");

	// Fullscreen-quad corners in NDC, in perimeter order (BL, BR, TR, TL).
	GLfloat quadVertices[] = {
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f
	};

	glGenVertexArrays(1, &sunQuadVAO);
	glBindVertexArray(sunQuadVAO);
	glGenBuffers(1, &sunQuadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, sunQuadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

	GLint vPositionLoc = glGetAttribLocation(sunShader, "vPosition");
	glEnableVertexAttribArray(vPositionLoc);
	glVertexAttribPointer(vPositionLoc, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), (GLvoid*)0);
	glBindVertexArray(0);
}

// Shared uniforms for both sun passes (same billboard, same projected centre
// and half-size). uDisc picks the fragment-shader mode: 1 = opaque disc,
// 0 = additive corona halo.
static void setupSunUniforms(mat4 proj, mat4 view, vec3 center, float radius, GLfloat uDisc)
{
	glUniformMatrix4fv(glGetUniformLocation(sunShader, "projection"), 1, GL_FALSE, &proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(sunShader, "view"), 1, GL_FALSE, &view.m[0][0]);
	glUniform1f(glGetUniformLocation(sunShader, "sunRadius"), radius);
	glUniform3f(glGetUniformLocation(sunShader, "sunCenter"), center.x, center.y, center.z);
	glUniform1f(glGetUniformLocation(sunShader, "systemTime"), (float)glfwGetTime());

	int viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glUniform1f(glGetUniformLocation(sunShader, "aspect"), (float)viewport[2] / (float)viewport[3]);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sunTexture);
	glUniform1i(glGetUniformLocation(sunShader, "tex"), 0);

	glUniform1f(glGetUniformLocation(sunShader, "uDisc"), uDisc);
}

// Draw as a fan: the perimeter-ordered corners (BL,BR,TR,TL) tile into two
// non-overlapping CCW triangles; a strip in that order leaves the top-left
// sector uncovered.
static void drawSunQuadPass(void)
{
	glBindVertexArray(sunQuadVAO);
	glDisable(GL_CULL_FACE);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glEnable(GL_CULL_FACE);
	glBindVertexArray(0);
}

void drawSunDisc(mat4 proj, mat4 view, vec3 center, float radius)
{
	glUseProgram(sunShader);
	setupSunUniforms(proj, view, center, radius, 1.0f);

	/* Main pass is reversed-Z (Main.c setReversedDepth): nearer = greater. */
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GREATER);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);

	drawSunQuadPass();
}

// Drawn LAST: depth-tested against the scene (so a planet in front still
// hides it) but never writes depth, so planets behind the halo stay visible
// and the glow simply blends over them.
void drawSunGlow(mat4 proj, mat4 view, vec3 center, float radius)
{
	glUseProgram(sunShader);
	setupSunUniforms(proj, view, center, radius, 0.0f);

	/* Additive, times (1 - destination alpha): the cloud cover Clouds_Draw
	   leaves there (Main.c), so the sun's glare goes behind thick cloud. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
	glDepthFunc(GL_GREATER);
	glDepthMask(GL_FALSE);

	drawSunQuadPass();

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}
