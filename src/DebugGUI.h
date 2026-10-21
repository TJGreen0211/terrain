#ifndef DEBUG_GUI_H
#define DEBUG_GUI_H
#include "Window.h"

/* Nuklear debug panel: a movable, resizable, collapsible window in the top
   right. Nuklear stays private to DebugGUI.c (its NK_INCLUDE_* config must
   match in every file that includes it), so callers only see this API.

   Input.c keeps ownership of the GLFW callbacks and forwards events here;
   DebugGUI_WantsMouse lets it skip the arcball / camera zoom while the
   cursor is over the panel. F1 toggles visibility. Sections edit the
   modules' live parameter structs directly (g_atmosphere, ...). */

void DebugGUI_Init(GLFWwindow *window); // after glad; claims the window user pointer
void DebugGUI_Frame(void);              // build + draw, into the bound (default) framebuffer
void DebugGUI_Shutdown(void);

void DebugGUI_ToggleVisible(void);
int  DebugGUI_ConsumeNewSystem(void);         // 1 once after the system seed changed (SolarSystem_Generate)
int  DebugGUI_ConsumeRegenerate(void);        // 1 once after "Regenerate planet" / "New seed"
int  DebugGUI_WantsMouse(double x, double y); // cursor in window px y-down

void DebugGUI_KeyEvent(GLFWwindow *window, int key, int scancode, int action, int mods);
void DebugGUI_CharEvent(GLFWwindow *window, unsigned int codepoint);
void DebugGUI_ScrollEvent(GLFWwindow *window, double xoffset, double yoffset);
void DebugGUI_MouseButtonEvent(GLFWwindow *window, int button, int action, int mods);

#endif
