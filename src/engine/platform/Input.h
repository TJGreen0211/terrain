#ifndef INPUT_H
#define INPUT_H
#include "engine/core/gl.h"

#include "engine/camera/arcballCamera.h"
#include "engine/platform/Window.h"

/* The UI layer that sees input before the camera (the app wires its debug
   panel in here). Input calls key/chr/scroll/mouseButton first for every
   event, skips the camera while wantsMouse says the cursor is over the UI,
   and F1 calls toggleVisible. Any member may be NULL. */
typedef struct InputUiHooks {
    void (*key)(GLFWwindow *window, int key, int scancode, int action, int mods);
    void (*chr)(GLFWwindow *window, unsigned int codepoint);
    void (*scroll)(GLFWwindow *window, double xoffset, double yoffset);
    void (*mouseButton)(GLFWwindow *window, int button, int action, int mods);
    int  (*wantsMouse)(double x, double y);
    void (*toggleVisible)(void);
} InputUiHooks;
void Input_SetUiHooks(const InputUiHooks *hooks);

void doMovement(float deltaTime);
int getWireframeState(void);
int getRotationPaused(void);
int consumeRegenRequest(void);
int consumeFocusStep(void);   /* Tab / Shift+Tab since the last call */

#endif
