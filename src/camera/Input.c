#include "Input.h"
#include "GUI.h"

/* Held state per movement key, so any combination (W+D, W+A, ...) moves at
   once and releasing one key leaves the others held. */
static int keyW, keyA, keyS, keyD;
static int wireframe = 0;
static int rotationPaused = 0;
static int regenRequested = 0;

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mode)
{
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GL_TRUE);
    if (key == GLFW_KEY_T && action == GLFW_PRESS && mode == 0)
        wireframe = !wireframe;
    if (key == GLFW_KEY_G && action == GLFW_PRESS && mode == 0)
        toggleSurfaceCamera();
    if (key == GLFW_KEY_R && action == GLFW_PRESS && mode == 0)
        rotationPaused = !rotationPaused;
    if (key == GLFW_KEY_H && action == GLFW_PRESS && mode == 0)
        regenRequested = 1;

    /* GLFW_REPEAT leaves the held state alone. */
    if (action != GLFW_PRESS && action != GLFW_RELEASE)
        return;
    int down = (action == GLFW_PRESS);
    if (key == GLFW_KEY_W) keyW = down;
    if (key == GLFW_KEY_S) keyS = down;
    if (key == GLFW_KEY_A) keyA = down;
    if (key == GLFW_KEY_D) keyD = down;
}

/* W/S forward/back, A/D left/right; opposite keys cancel. */
void doMovement(float deltaTime) {
    float forward = (float)(keyW - keyS);
    float right = (float)(keyD - keyA);
    processKeyboard(forward, right, deltaTime);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
	processMouseScroll((float)yoffset);
}

/* Left press over a camera slider grabs it; the grab owns the drag until the
   release, so the arcball never sees that press. Otherwise the press starts
   an arcball drag and the release ends it, the moment each happens. */
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
	if (button != GLFW_MOUSE_BUTTON_LEFT)
		return;
	if (action == GLFW_PRESS) {
		double mx, my;
		int width, height;
		glfwGetCursorPos(window, &mx, &my);
		glfwGetWindowSize(window, &width, &height);
		uiSliderPress((float)mx, (float)my);
		if (!uiSliderGrabbed())
			arcballBegin(mx, my, width, height);
	}
	else if (action == GLFW_RELEASE) {
		uiSliderRelease();
		arcballEnd();
	}
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
	uiSetMouse((float)xpos, (float)ypos);
	if (uiSliderGrabbed() || !arcballDragging())
		return;
	int width, height;
	glfwGetWindowSize(window, &width, &height);
	arcballDrag(xpos, ypos, width, height);
}

int getWireframeState(void) {
	return wireframe;
}

int getRotationPaused(void) {
	return rotationPaused;
}

int consumeRegenRequest(void) {
	int r = regenRequested;
	regenRequested = 0;
	return r;
}
