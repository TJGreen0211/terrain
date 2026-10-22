#ifndef DEBUG_GUI_H
#define DEBUG_GUI_H
#include "engine/platform/window.h"

/* Nuklear debug panel: a movable, resizable, collapsible window in the top
   right, hosting one collapsible tab per registered panel (below the
   built-in Performance tab), in registration order. Each module draws its
   own tab (X_DebugPanel) with nuklear through engine/debug/nk.h and the
   row helpers below.

   input.c keeps ownership of the GLFW callbacks and forwards events here
   through the InputUiHooks the app registers (Input_SetUiHooks);
   DebugGUI_WantsMouse lets it skip the arcball / camera zoom while the
   cursor is over the panel. F1 toggles visibility. */

struct nk_context;

/* Draws one tab's contents (called only while the tab is open). */
typedef void (*DebugGUI_PanelFn)(struct nk_context *ctx);
/* Called once, on the panel's first frame (before any tab draws): where a
   panel snapshots the startup values its reset buttons restore. */
typedef void (*DebugGUI_StartupFn)(void);

void DebugGUI_Init(GLFWwindow *window); // after glad; claims the window user pointer
/* title must outlive the panel; startup may be NULL. */
void DebugGUI_AddPanel(const char *title, DebugGUI_PanelFn draw, DebugGUI_StartupFn startup);
void DebugGUI_Frame(void);              // build + draw, into the bound (default) framebuffer
void DebugGUI_Shutdown(void);

void DebugGUI_ToggleVisible(void);
int  DebugGUI_WantsMouse(double x, double y); // cursor in window px y-down

void DebugGUI_KeyEvent(GLFWwindow *window, int key, int scancode, int action, int mods);
void DebugGUI_CharEvent(GLFWwindow *window, unsigned int codepoint);
void DebugGUI_ScrollEvent(GLFWwindow *window, double xoffset, double yoffset);
void DebugGUI_MouseButtonEvent(GLFWwindow *window, int button, int action, int mods);

/* ---- Row helpers for panels ---- */
/* Widget row height (px), from the font size. */
float DebugGUI_RowHeight(void);
/* Starts a name | widget | value row (the slider rows' layout). */
void DebugGUI_RowLayout(struct nk_context *ctx);
/* One tunable per row: name | slider | value. Nuklear's property widgets
   print 2 decimals (NK_MAX_FLOAT_PRECISION, not overridable), too few for
   values like K_M, so the value is a label with its own format. */
void DebugGUI_Slider(struct nk_context *ctx, const char *name, float *value,
                     float lo, float hi, float step, const char *fmt);
void DebugGUI_SliderInt(struct nk_context *ctx, const char *name, int *value, int lo, int hi);
/* Muted sub-heading between groups of rows. */
void DebugGUI_Group(struct nk_context *ctx, const char *text);

#endif
