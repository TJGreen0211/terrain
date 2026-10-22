#include "world/bodies/BodyDraw.h"
#include "world/shadows/Shadows.h"
#include "world/terrain/QuadTree.h"
#include "engine/gfx/Common.h"

static GLuint litProgram;

void BodyDraw_Init(void)
{
	createShader(&litProgram, "world/bodies/light.vert",
		"world/bodies/light.frag");
}

GLuint BodyDraw_Program(void) { return litProgram; }

static void setMVP(GLuint shader, mat4 m, const FrameContext *f)
{
	glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, &f->proj.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "model"), 1, GL_FALSE, &m.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(shader, "view"), 1, GL_FALSE, &f->view.m[0][0]);
}

void BodyDraw_SunMarker(GLuint vao, int vertices, GLuint texture, const Body *sun, const FrameContext *f)
{
	GLuint shader = litProgram;
	glUseProgram(shader);
	glBindVertexArray (vao);

	mat4 model = dmat4tomat4(Body_ModelMatrix(sun, f->cam));
	setMVP(shader, model, f);

	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	/* light.frag always samples the shadow map; the marker sits in front of
	   the light frustum's near plane, so it always reads as lit. */
	Shadows_BindMaps(shader, 1);
	glDrawArrays(GL_TRIANGLES, 0, vertices);
	glBindVertexArray(0);
}

/* The light.vert instance path computes the equirectangular UVs that
   initQuadCube used to bake, so moon.jpg frames it unchanged. Only the
   uniforms the Moon always set are touched; `instanced` is reset afterwards
   because the program is shared with the sun marker. */
void BodyDraw_Moon(GLuint texture, const Body *moon, const FrameContext *f)
{
	GLuint shader = litProgram;
	glUseProgram(shader);

	/* Smooth sphere: no relief, the surface itself is the horizon occluder. */
	QuadTree_Body body = { Body_ModelMatrix(moon, f->cam), moon->radius, NULL, 0.0 };
	mat4 model = dmat4tomat4(body.model);
	setMVP(shader, model, f);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 1);

	// Camera-relative space: the camera is exactly the origin.
	glUniform3f(glGetUniformLocation(shader, "cameraPos"), 0.0f, 0.0f, 0.0f);
	glUniform3f(glGetUniformLocation(shader, "lightPosition"), f->lightPos.x, f->lightPos.y, f->lightPos.z);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shader, "texture1"), 0);
	/* Receives the planet's shadow when it passes behind it. */
	Shadows_BindMaps(shader, 1);
	QuadTree_DrawSphere(&body, f->viewProj);
	glUniform1i(glGetUniformLocation(shader, "instanced"), 0);
}
