#ifndef WINDOW_H
#define WINDOW_H
#include "engine/core/gl.h"

/* fixedWidth/fixedHeight > 0: a hidden window of exactly that size (bench
   runs); 0: a visible window the size of the primary monitor. */
GLFWwindow *setupGLFW(const char *title, int fixedWidth, int fixedHeight);
float getWindowWidth();
float getWindowHeight();

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mode);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void char_callback(GLFWwindow* window, unsigned int codepoint);

#endif