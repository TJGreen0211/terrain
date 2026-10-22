#include "Common.h"
#include "CommonGL.h"
#include "Skybox.h"
#include "Nebula.h"
#include <stdio.h>

static GLuint skyboxShader;
static GLuint skyboxVAO, skyboxVBO;

// Six faces in the order loadCubemap expects: faces[i] goes to
// CUBE_MAP_POSITIVE_X+i, so right, left, top, bottom, back, front are
// +X, -X, +Y, -Y, +Z, -Z (the nebula bake writes them in this order).
// The app chdir's to src, so assets live in ../assets.
static const char *faceNames[6] = { "right", "left", "top", "bottom", "back", "front" };

// The nebula sky set g_nebulaSkyName (Nebula.c), with its settings, when
// all six faces are there; 0 otherwise.
static GLuint loadNebulaSky(void)
{
	if (!g_nebulaSkyName[0])
		return 0;
	char paths[6][256];
	char *faces[6];
	for (int i = 0; i < 6; i++) {
		snprintf(paths[i], sizeof paths[i], "../assets/textures/nebula_%s/%s.png", g_nebulaSkyName, faceNames[i]);
		FILE *f = fopen(paths[i], "rb");
		if (!f) {
			fprintf(stderr, "[skybox] no nebula sky '%s' (%s); using skybox3\n", g_nebulaSkyName, paths[i]);
			return 0;
		}
		fclose(f);
		faces[i] = paths[i];
	}
	// The runtime wisps use the settings the sky was baked with. The baked
	// glow crosses face edges: filter across them too (only for a nebula
	// sky, so skybox3 samples as it always has).
	Nebula_LoadParams(g_nebulaSkyName);
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	return loadCubemap(faces);
}

GLuint loadSkyboxCube(void)
{
	GLuint nebula = loadNebulaSky();
	if (nebula)
		return nebula;
	char *cubemapArray[] = {
		"../assets/textures/skybox3/right.png",
		"../assets/textures/skybox3/left.png",
		"../assets/textures/skybox3/top.png",
		"../assets/textures/skybox3/bottom.png",
		"../assets/textures/skybox3/back.png",
		"../assets/textures/skybox3/front.png"
	};
	return loadCubemap(cubemapArray);
}

void initSkybox(void)
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

	createShader(&skyboxShader, "shaders/skybox.vert", "shaders/skybox.frag");

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

void drawSkybox(GLuint cubemap, mat4 projection, mat4 viewRotation)
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
