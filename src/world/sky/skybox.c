#include "engine/gfx/shader.h"
#include "engine/gfx/framebuffer.h"
#include "world/sky/skybox.h"
#include "world/sky/nebula.h"
#include "engine/core/paths.h"
#include <stdio.h>

static GLuint skyboxShader;
static GLuint skyboxVAO, skyboxVBO;

// Six faces in the order Texture_LoadCubemap expects: faces[i] goes to
// CUBE_MAP_POSITIVE_X+i, so right, left, top, bottom, back, front are
// +X, -X, +Y, -Y, +Z, -Z (the nebula bake writes them in this order).
static const char *faceNames[6] = { "right", "left", "top", "bottom", "back", "front" };

// The nebula sky set Nebula_SkyName() (nebula.c), with its settings, when
// all six faces are there; 0 otherwise.
static GLuint loadNebulaSky(void)
{
	if (!Nebula_SkyName()[0])
		return 0;
	char paths[6][256];
	char *faces[6];
	for (int i = 0; i < 6; i++) {
		Paths_Asset(paths[i], sizeof paths[i], "textures/nebula_%s/%s.png", Nebula_SkyName(), faceNames[i]);
		FILE *f = fopen(paths[i], "rb");
		if (!f) {
			fprintf(stderr, "[skybox] no nebula sky '%s' (%s); using skybox3\n", Nebula_SkyName(), paths[i]);
			return 0;
		}
		fclose(f);
		faces[i] = paths[i];
	}
	// The runtime wisps use the settings the sky was baked with. The baked
	// glow crosses face edges: filter across them too. (The planet bake
	// turns seamless filtering on for every sky anyway: terrain_gen.c.)
	Nebula_LoadParams(Nebula_SkyName());
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	return Texture_LoadCubemap(faces);
}

GLuint Skybox_LoadCube(void)
{
	GLuint nebula = loadNebulaSky();
	if (nebula)
		return nebula;
	char paths[6][256];
	char *cubemapArray[6];
	for (int i = 0; i < 6; i++)
		cubemapArray[i] = Paths_Asset(paths[i], sizeof paths[i], "textures/skybox3/%s.png", faceNames[i]);
	return Texture_LoadCubemap(cubemapArray);
}

void Skybox_Init(void)
{
	// Cube corners (36 triangles, inward-facing so GL_BACK culling keeps them).
	GLfloat skyboxVertices[] = {
		// back
		-1.0f, 1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, 1.0f, -1.0f,
		-1.0f, 1.0f, -1.0f,

		// left
		-1.0f, -1.0f, 1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f, 1.0f, -1.0f,
		-1.0f, 1.0f, -1.0f,
		-1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 1.0f,

		// right
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, 1.0f,
		 1.0f, 1.0f, 1.0f,
		 1.0f, 1.0f, 1.0f,
		 1.0f, 1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,

		// top
		-1.0f, -1.0f, 1.0f,
		-1.0f, 1.0f, 1.0f,
		 1.0f, 1.0f, 1.0f,
		 1.0f, 1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f,
		-1.0f, -1.0f, 1.0f,

		// bottom
		-1.0f, 1.0f, -1.0f,
		 1.0f, 1.0f, -1.0f,
		 1.0f, 1.0f, 1.0f,
		 1.0f, 1.0f, 1.0f,
		-1.0f, 1.0f, 1.0f,
		-1.0f, 1.0f, -1.0f,

		// front
		-1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f,
		 1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f
	};

	Shader_Create(&skyboxShader, "world/sky/skybox.vert", "world/sky/skybox.frag");

	glGenVertexArrays(1, &skyboxVAO);
	glBindVertexArray(skyboxVAO);
	glGenBuffers(1, &skyboxVBO);
	glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);

	// The shader declares `in vec4` but only 3 floats are provided, so w is
	// implicitly 1.0 (the clip w the pos.xyww trick relies on).
	GLint position = glGetAttribLocation(skyboxShader, "vPosition");
	glEnableVertexAttribArray(position);
	glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), (GLvoid*)0);
	glBindVertexArray(0);
}

void Skybox_Draw(GLuint cubemap, mat4 projection, mat4 viewRotation)
{
	glUseProgram(skyboxShader);

	// Far-plane background layer: the box sits at z==1.0 (xyww trick) and the
	// depth buffer was cleared to 1.0; the scene redraws over it next. Draw
	// with the depth TEST and culling disabled so no fragment is rejected
	// (avoids winding/precision edge cases), and never write depth; the scene
	// re-enables the depth test afterwards. Draw this first, before the scene.
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	glUniformMatrix4fv(glGetUniformLocation(skyboxShader, "projection"), 1, GL_FALSE, &projection.m[0][0]);
	glUniformMatrix4fv(glGetUniformLocation(skyboxShader, "ModelView"), 1, GL_FALSE, &viewRotation.m[0][0]);

	glBindVertexArray(skyboxVAO);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(glGetUniformLocation(skyboxShader, "skybox"), 0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
	glDrawArrays(GL_TRIANGLES, 0, 36);
	glBindVertexArray(0);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
}
