#include "Window.h"

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mode);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void char_callback(GLFWwindow* window, unsigned int codepoint);

static float windowWidth = 1.0f, windowHeight = 1.0f;

/* Minimizing reports 0x0; keep the last real size so the projection aspect
   never divides by zero. */
static void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
	(void)window;
	if (width > 0 && height > 0) {
		windowWidth = (float)width;
		windowHeight = (float)height;
	}
}

GLFWwindow *setupGLFW() {
	glfwInit();
	glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	/* No multisampling on the window itself: the scene renders into its own
	   4x MSAA float-depth target (Main.c) and is resolved into this one. */
	glfwWindowHint(GLFW_SAMPLES, 0);
	
	/* Initial size: the primary monitor's mode (the only video-mode query). */
	const GLFWvidmode *mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	windowWidth = (float)mode->width;
	windowHeight = (float)mode->height;
	GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "Shadows", NULL, NULL);
	if (window == NULL)
		return NULL;
	
	glfwMakeContextCurrent(window);
	/* The OS may clamp the window (taskbar, decorations): start from the
	   real framebuffer, then track resizes. */
	int fbw, fbh;
	glfwGetFramebufferSize(window, &fbw, &fbh);
	framebuffer_size_callback(window, fbw, fbh);
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	//glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_ENABLED);
	glfwSetKeyCallback(window, key_callback);
	glfwSetCursorPosCallback(window, mouse_callback);
	glfwSetScrollCallback(window, scroll_callback);
	glfwSetMouseButtonCallback(window, mouse_button_callback);
	glfwSetCharCallback(window, char_callback);
	return window;
}

/* Cached framebuffer size: these are called many times per frame, and
   glfwGetVideoMode is an uncached EnumDisplaySettingsW call on Win32. */
float getWindowHeight() {
	return windowHeight;
}

float getWindowWidth() {
	return windowWidth;
}