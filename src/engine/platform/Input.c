#include "engine/platform/Input.h"
#include "engine/debug/GUI.h"

/* Held state per movement key, so any combination (W+D, W+A, ...) moves at
   once and releasing one key leaves the others held. */
static int keyW, keyA, keyS, keyD;
static int wireframe = 0;
static int rotationPaused = 0;
static int regenRequested = 0;
static int focusStep = 0;

static InputUiHooks ui;

void Input_SetUiHooks(const InputUiHooks *hooks)
{
    ui = *hooks;
}

static int uiWantsMouse(double x, double y)
{
    return ui.wantsMouse && ui.wantsMouse(x, y);
}

void Input_KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mode)
{
    if (ui.key) ui.key(window, key, scancode, action, mode);
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GL_TRUE);
    if (key == GLFW_KEY_T && action == GLFW_PRESS && mode == 0)
        wireframe = !wireframe;
    if (key == GLFW_KEY_G && action == GLFW_PRESS && mode == 0)
        Camera_ToggleSurface();
    if (key == GLFW_KEY_R && action == GLFW_PRESS && mode == 0)
        rotationPaused = !rotationPaused;
    if (key == GLFW_KEY_H && action == GLFW_PRESS && mode == 0)
        regenRequested = 1;
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS && mode == 0 && ui.toggleVisible)
        ui.toggleVisible();
    /* Tab / Shift+Tab: next / previous focus body. */
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS)
        focusStep += (mode & GLFW_MOD_SHIFT) ? -1 : 1;

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
void Input_ApplyMovement(float deltaTime) {
    float forward = (float)(keyW - keyS);
    float right = (float)(keyD - keyA);
    Camera_ProcessKeyboard(forward, right, deltaTime);
}

void Input_CharCallback(GLFWwindow* window, unsigned int codepoint) {
	if (ui.chr) ui.chr(window, codepoint);
}

/* The wheel scrolls the debug panel while over it, zooms the camera otherwise. */
void Input_ScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
	double mx, my;
	if (ui.scroll) ui.scroll(window, xoffset, yoffset);
	glfwGetCursorPos(window, &mx, &my);
	if (uiWantsMouse(mx, my))
		return;
	Camera_ProcessScroll((float)yoffset);
}

/* Left press over the debug panel belongs to nuklear (it polls the button
   itself). Over a camera slider it grabs the slider; the grab owns the drag
   until the release, so the arcball never sees that press. Otherwise the
   press starts an arcball drag and the release ends it, the moment each
   happens. */
void Input_MouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
	if (ui.mouseButton) ui.mouseButton(window, button, action, mods);
	if (button != GLFW_MOUSE_BUTTON_LEFT)
		return;
	if (action == GLFW_PRESS) {
		double mx, my;
		int width, height;
		glfwGetCursorPos(window, &mx, &my);
		if (uiWantsMouse(mx, my))
			return;
		glfwGetWindowSize(window, &width, &height);
		UI_SliderPress((float)mx, (float)my);
		if (!UI_SliderGrabbed())
			Camera_ArcballBegin(mx, my, width, height);
	}
	else if (action == GLFW_RELEASE) {
		UI_SliderRelease();
		Camera_ArcballEnd();
	}
}

void Input_CursorCallback(GLFWwindow* window, double xpos, double ypos) {
	UI_SetMouse((float)xpos, (float)ypos);
	if (UI_SliderGrabbed() || !Camera_ArcballDragging())
		return;
	int width, height;
	glfwGetWindowSize(window, &width, &height);
	Camera_ArcballDrag(xpos, ypos, width, height);
}

int Input_Wireframe(void) {
	return wireframe;
}

int Input_RotationPaused(void) {
	return rotationPaused;
}

int Input_ConsumeRegenRequest(void) {
	int r = regenRequested;
	regenRequested = 0;
	return r;
}

/* Focus-body steps requested since the last call (Tab +1, Shift+Tab -1). */
int Input_ConsumeFocusStep(void) {
	int s = focusStep;
	focusStep = 0;
	return s;
}
