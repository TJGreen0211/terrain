#include "engine/gfx/scene_target.h"
#include "engine/gfx/shader.h"
#include <stdio.h>

static GLuint fbo, colorTex, depthTex, resolveProgram, emptyVAO;
static int samples = 1, targetW, targetH;

void SceneTarget_Init(int sampleCount)
{
	samples = sampleCount > 0 ? sampleCount : 1;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &colorTex);
	glGenTextures(1, &depthTex);
	glGenVertexArrays(1, &emptyVAO);
	Shader_Create(&resolveProgram, "engine/gfx/resolve.vert", "engine/gfx/resolve.frag");
}

int SceneTarget_Ensure(int w, int h)
{
	if (targetW == w && targetH == h)
		return 0;
	targetW = w;
	targetH = h;
	/* Fixed sample locations on both: required to mix them in one FBO. */
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, colorTex);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, w, h, GL_TRUE);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, depthTex);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_DEPTH_COMPONENT32F, w, h, GL_TRUE);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, colorTex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, depthTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[scene_target] framebuffer incomplete\n");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return 1;
}

GLuint SceneTarget_Framebuffer(void) { return fbo; }
GLuint SceneTarget_DepthTexture(void) { return depthTex; }
int SceneTarget_Samples(void) { return samples; }

void SceneTarget_Resolve(int w, int h)
{
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glViewport(0, 0, w, h);

	glUseProgram(resolveProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, colorTex);
	glUniform1i(glGetUniformLocation(resolveProgram, "scene"), 0);
	glUniform1i(glGetUniformLocation(resolveProgram, "samples"), samples);
	glBindVertexArray(emptyVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

	if (blend) glEnable(GL_BLEND);
	if (depthTest) glEnable(GL_DEPTH_TEST);
	if (cull) glEnable(GL_CULL_FACE);
}
