#ifndef GUI_H
#define GUI_H
#include "engine/gfx/Common.h"

/* Slider subset of the OpenGLSolar GUI (ui/GUI.c): same API names, so its
   buttons / text can be ported later without touching callers. */

/* Horizontal slider. t is the handle position in [0,1]; the slider itself
   knows nothing about units — the change callback maps t to the real value.
   Pressing jumps to and grabs the handle; moving while grabbed updates t;
   releasing the mouse (anywhere) ends the grab. accent colours the fill and
   handle so each slider is distinguishable at a glance.
   rect is in window content pixels, y-down from the top-left (the same space
   as glfwGetCursorPos). */
typedef struct uiSlider {
	int id;
	float rect[4];   // interaction area, window px y-down
	float accent[3]; // fill + handle colour, rgb 0..1
	float t;         // 0..1
	int hovered;
	int active;
} uiSlider;

#define UI_MAX_SLIDERS 8

typedef void (*uiSliderChangedFn)(int id, float t);

void attachGUIShaders();
int  uiInit(void);
int  uiAddSlider(int id, float t0, const float accent[3]);
int  uiSliderCount(void);
void uiLayoutSliders(int winW, int winH, int count);
void uiSliderSetChangedFn(uiSliderChangedFn fn);
int  uiSliderPress(float x, float y); // grabs a slider under the cursor, returns its id or -1
int  uiSliderRelease(void);           // ends an active grab, returns the slider id or -1
int  uiSliderGrabbed(void);           // 1 while a slider is being dragged
void uiSetMouse(float x, float y);
void uiDrawAll(void);
void uiShutdown(void);

#endif
