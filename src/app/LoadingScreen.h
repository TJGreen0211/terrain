#ifndef LOADINGSCREEN_H
#define LOADINGSCREEN_H

#include <GLFW/glfw3.h>

// Setup: point the loading frames at the app's GL context, build the shader and
// quad, and arm the step counter. Called once in main() right after GLAD loads.
void LoadingScreen_Init(GLFWwindow *window);
// One loading step: repaint the bar at the current fraction done. The step
// callback every loader receives.
void LoadingScreen_Step(void);
// Draw one loading frame at the given 0..1 progress (used for the 0.0 start and
// the forced 1.0 final full-bar frame).
void LoadingScreen_Render(float progress);
// Re-arm for a later load (a planet bake on approach): `steps` Step calls
// fill the bar. End with LoadingScreen_Finish.
void LoadingScreen_Begin(GLFWwindow *window, int steps);
// Forced final full-bar frame, then detach the window.
void LoadingScreen_Finish(void);

#endif
