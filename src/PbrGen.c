#include "Main.h"
#include "PbrGen.h"
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define PBR_TEX_SIZE   512
#define PBR_NUM_FACES  6
#define PBR_NUM_PASSES 6

static const char *faceNames[PBR_NUM_FACES] = { "+Z", "-Z", "+X", "-X", "+Y", "-Y" };
static const char *mapNames[PBR_NUM_PASSES] = { "albedo", "height", "normal", "roughness", "metallic", "ao" };

static GLuint pbrVAO, pbrVBO, pbrIBO, pbrProgram, pbrFBO, pbrTexImage;
static int pbrIndexCount = 0;

static void mkdirs(char *path)
{
	for (int i = 1; path[i] != '\0'; i++) {
		if (path[i] == '/' || path[i] == '\\') {
			char saved = path[i];
			path[i] = '\0';
			_mkdir(path);
			path[i] = saved;
		}
	}
	_mkdir(path);
}

static GLuint initPbrGenShader()
{
	GLuint shader;
	createShader(&shader, "shaders/pbrgen.vert", "shaders/pbrgen.frag");
	return shader;
}

// Dense indexed triangle grid covering the viewport; the vertex shader maps
// it onto one cube face of the planet sphere for full-coverage 1:1 texel output.
static void initPbrGrid()
{
	int n = PBR_TEX_SIZE;
	int verts = n * n;
	int quads = (n - 1) * (n - 1);
	float *grid = malloc((size_t)verts * 2 * sizeof(float));
	unsigned int *indices = malloc((size_t)quads * 6 * sizeof(unsigned int));
	int i = 0;
	for (int j = 0; j < n; j++) {
		for (int k = 0; k < n; k++) {
			grid[i++] = (float)k / (float)(n - 1);
			grid[i++] = (float)j / (float)(n - 1);
		}
	}
	int o = 0;
	for (int j = 0; j < n - 1; j++) {
		for (int k = 0; k < n - 1; k++) {
			unsigned int id00 = (unsigned int)(j * n + k);
			unsigned int id01 = (unsigned int)(j * n + (k + 1));
			unsigned int id10 = (unsigned int)((j + 1) * n + k);
			unsigned int id11 = (unsigned int)((j + 1) * n + (k + 1));
			indices[o++] = id00; indices[o++] = id10; indices[o++] = id11;
			indices[o++] = id00; indices[o++] = id11; indices[o++] = id01;
		}
	}
	pbrIndexCount = o;
	glGenVertexArrays(1, &pbrVAO);
	glBindVertexArray(pbrVAO);
	glGenBuffers(1, &pbrVBO);
	glBindBuffer(GL_ARRAY_BUFFER, pbrVBO);
	glBufferData(GL_ARRAY_BUFFER, (size_t)verts * 2 * sizeof(float), grid, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
	glEnableVertexAttribArray(0);

	glGenBuffers(1, &pbrIBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pbrIBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)quads * 6 * sizeof(unsigned int), indices, GL_STATIC_DRAW);

	glBindVertexArray(0);
	free(grid);
	free(indices);
}

static void initPbrFBO()
{
	glGenTextures(1, &pbrTexImage);
	glBindTexture(GL_TEXTURE_2D, pbrTexImage);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PBR_TEX_SIZE, PBR_TEX_SIZE, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &pbrFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, pbrFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pbrTexImage, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[PBR] framebuffer incomplete: 0x%x\n", (unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER));
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void generatePlanetPbrTextures()
{
	pbrProgram = initPbrGenShader();
	initPbrGrid();
	initPbrFBO();

	GLubyte *pixels = malloc((size_t)PBR_TEX_SIZE * PBR_TEX_SIZE * 4);
	int written = 0;

	// Save/restore GL state: this is a full-screen render into a small FBO
	glViewport(0, 0, PBR_TEX_SIZE, PBR_TEX_SIZE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	for (int f = 0; f < PBR_NUM_FACES; f++) {
		char dir[256];
		snprintf(dir, sizeof dir, "../assets/planet/pbr/%s", faceNames[f]);
		mkdirs(dir);

		for (int p = 0; p < PBR_NUM_PASSES; p++) {
			glBindFramebuffer(GL_FRAMEBUFFER, pbrFBO);
			glUseProgram(pbrProgram);
			glUniform1i(glGetUniformLocation(pbrProgram, "faceIndex"), f);
			glUniform1i(glGetUniformLocation(pbrProgram, "mapPass"), p);
			glBindVertexArray(pbrVAO);
			glClear(GL_COLOR_BUFFER_BIT);
			glDrawElements(GL_TRIANGLES, pbrIndexCount, GL_UNSIGNED_INT, NULL);
			glReadPixels(0, 0, PBR_TEX_SIZE, PBR_TEX_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

			// FBO row 0 is the bottom of the image, PNG row 0 is the top: flip vertically
			for (int r = 0; r < PBR_TEX_SIZE / 2; r++) {
				GLubyte *a = pixels + (size_t)r * PBR_TEX_SIZE * 4;
				GLubyte *b = pixels + (size_t)(PBR_TEX_SIZE - 1 - r) * PBR_TEX_SIZE * 4;
				for (int c = 0; c < PBR_TEX_SIZE * 4; c += 4) {
					GLubyte ra = a[c+0], ga = a[c+1], ba = a[c+2], aa = a[c+3];
					a[c+0] = b[c+0]; a[c+1] = b[c+1]; a[c+2] = b[c+2]; a[c+3] = b[c+3];
					b[c+0] = ra; b[c+1] = ga; b[c+2] = ba; b[c+3] = aa;
				}
			}

			char path[300];
			snprintf(path, sizeof path, "../assets/planet/pbr/%s/%s.png", faceNames[f], mapNames[p]);
			if (stbi_write_png(path, PBR_TEX_SIZE, PBR_TEX_SIZE, 4, pixels, PBR_TEX_SIZE * 4))
				written++;
			else
				fprintf(stderr, "[PBR] failed to write %s\n", path);
		}
	}

	glViewport(0, 0, getWindowWidth(), getWindowHeight());
	glBindVertexArray(0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	free(pixels);

	fprintf(stderr, "[PBR] wrote %d/%d textures to ../assets/planet/pbr\n",
	        written, PBR_NUM_FACES * PBR_NUM_PASSES);
}
