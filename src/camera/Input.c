#include "Input.h"

static int actionPress, keys;
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
	if(action == GLFW_PRESS)
    	actionPress = GLFW_PRESS;
    else if(action == GLFW_RELEASE)
    	actionPress = 0;

    if (key == GLFW_KEY_W && action == GLFW_PRESS){
    	keys = GLFW_KEY_W;
    }
    if (key == GLFW_KEY_S && action == GLFW_PRESS){
    	keys = GLFW_KEY_S;
    }
    if (key == GLFW_KEY_A && action == GLFW_PRESS){
    	keys = GLFW_KEY_A;
    }
    if (key == GLFW_KEY_D && action == GLFW_PRESS){
    	keys = GLFW_KEY_D;
    }
    if(action == GLFW_RELEASE)
    	keys = 0;
}

void doMovement(float deltaTime) {
	float deltaSpeed = 1.0;
	if(keys == GLFW_KEY_W && actionPress == GLFW_PRESS)
        processKeyboard(FORWARD, deltaTime, deltaSpeed);
    if(keys == GLFW_KEY_S && actionPress == GLFW_PRESS)
        processKeyboard(BACKWARD, deltaTime, deltaSpeed);
    if(keys == GLFW_KEY_A && actionPress == GLFW_PRESS)
        processKeyboard(LEFT, deltaTime, deltaSpeed);
    if(keys == GLFW_KEY_D && actionPress == GLFW_PRESS)
        processKeyboard(RIGHT, deltaTime, deltaSpeed);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
	float mouseZoomOffset = 0.0f;
	mouseZoomOffset = processMouseScroll(yoffset);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
	int state = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT);
	//xpos = 1.0*xpos/getWindowWidth()*2 - 1.0;
	//ypos =  1.0*ypos/getWindowHeight()*2 - 1.0;
	if (state == GLFW_PRESS)
	{
		processMouseMovement(xpos, ypos, 0);
	}
	else {
		processMouseMovement(xpos, ypos, 1);
	}
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
