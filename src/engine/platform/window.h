#ifndef WINDOW_H
#define WINDOW_H
#include "engine/core/gl.h"

/* fixedWidth/fixedHeight > 0: a hidden window of exactly that size (bench
   runs); 0: a visible window the size of the primary monitor. */
GLFWwindow *Window_Create(const char *title, int fixedWidth, int fixedHeight);
int Window_Width();
int Window_Height();
/* Puts text on the system clipboard (the current window's). */
void Window_SetClipboard(const char *text);

#endif