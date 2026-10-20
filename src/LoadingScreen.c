#include "Common.h"
#include "LoadingScreen.h"
#include "TerrainGen.h"

static GLFWwindow *loadWindow = NULL;
static GLuint loadingShader, loadingVAO, loadingVBO;
static int loadStepDone = 0;

// Number of discrete load steps main() takes: waves (1) + all shaders (1) +
// quadtree (1) + sun (1) + skybox setup (1) + skybox cube (1) + textures (1) +
// sphere/atmosphere/ring (1) + OBJ models (3) + quad cube (1) + framebuffers
// (1) + the heightmap bake's own steps (TERRAIN_BAKE_STEPS). Steps are grouped
// so each covers a roughly similar slice of load time. If the count drifts the
// bar just tops out early or late, and the final frame forces it to 1.0.
#define LOAD_TOTAL_STEPS (1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 3 + 1 + 1 + TERRAIN_BAKE_STEPS)

void LoadingScreen_Init(GLFWwindow *window)
{
	loadWindow = window;

	createShader(&loadingShader, "shaders/loading.vert",
		"shaders/loading.frag");

	GLfloat quadVertices[] = {
		-1.0f, -1.0f, 0.0f,
		1.0f, -1.0f, 0.0f,
		1.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f
	};

	glGenVertexArrays(1, &loadingVAO);
	glBindVertexArray(loadingVAO);
	glGenBuffers(1, &loadingVBO);
	glBindBuffer(GL_ARRAY_BUFFER, loadingVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

	GLint vPositionLoc = glGetAttribLocation(loadingShader, "vPosition");
	glEnableVertexAttribArray(vPositionLoc);
	glVertexAttribPointer(vPositionLoc, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), (GLvoid*)0);
	glBindVertexArray(0);

	loadStepDone = 0;
}

void LoadingScreen_Render(float progress)
{
	if (!loadWindow)
		return;

	glfwPollEvents();

	int fbw, fbh;
	glfwGetFramebufferSize(loadWindow, &fbw, &fbh);
	glViewport(0, 0, fbw, fbh);

	glDisable(GL_DEPTH_TEST);
	glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(loadingShader);
	glUniform1f(glGetUniformLocation(loadingShader, "uProgress"), progress);
	glUniform1f(glGetUniformLocation(loadingShader, "uTime"), (float)glfwGetTime());
	glBindVertexArray(loadingVAO);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glBindVertexArray(0);
	glEnable(GL_DEPTH_TEST);

	glfwSwapBuffers(loadWindow);
}

// One loading step: repaint the bar at the current fraction done. Call once
// per discrete unit of work (each texture decode, each init phase).
void LoadingScreen_Step(void)
{
	float pct = (float)loadStepDone / (float)LOAD_TOTAL_STEPS;
	loadStepDone++;
	if (pct > 1.0f)
		pct = 1.0f;
	LoadingScreen_Render(pct);
}

void LoadingScreen_Finish(void)
{
	LoadingScreen_Render(1.0f);
	loadWindow = NULL;
}
