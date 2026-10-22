#include "engine/platform/Window.h"
#include "engine/platform/Input.h"

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

GLFWwindow *Window_Create(const char *title, int fixedWidth, int fixedHeight) {
	glfwInit();
	glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	/* No multisampling on the window itself: the scene renders into its own
	   4x MSAA float-depth target (Main.c) and is resolved into this one. */
	glfwWindowHint(GLFW_SAMPLES, 0);
	
	/* A fixed size (bench runs) renders into a hidden window: identical
	   resolution and pixel budget for every build. */
	int width = fixedWidth, height = fixedHeight;
	if (fixedWidth > 0 && fixedHeight > 0)
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	else {
		/* Initial size: the primary monitor's mode (the only video-mode query). */
		const GLFWvidmode *mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
		width = mode->width;
		height = mode->height;
	}
	windowWidth = (float)width;
	windowHeight = (float)height;
	GLFWwindow* window = glfwCreateWindow(width, height, title, NULL, NULL);
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
	glfwSetKeyCallback(window, Input_KeyCallback);
	glfwSetCursorPosCallback(window, Input_CursorCallback);
	glfwSetScrollCallback(window, Input_ScrollCallback);
	glfwSetMouseButtonCallback(window, Input_MouseButtonCallback);
	glfwSetCharCallback(window, Input_CharCallback);
	return window;
}

/* Cached framebuffer size: these are called many times per frame, and
   glfwGetVideoMode is an uncached EnumDisplaySettingsW call on Win32. */
float Window_Height() {
	return windowHeight;
}

float Window_Width() {
	return windowWidth;
}