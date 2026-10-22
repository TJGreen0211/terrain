#ifndef UI_H
#define UI_H
#include "engine/gfx/shader.h"

/* Slider subset of the OpenGLSolar GUI (ui/ui.c): same API names, so its
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

void UI_InitShaders();
int  UI_Init(void);
int  UI_AddSlider(int id, float t0, const float accent[3]);
void UI_LayoutSliders(int winW, int winH, int count);
void UI_SetSliderChangedFn(uiSliderChangedFn fn);
int  UI_SliderPress(float x, float y); // grabs a slider under the cursor, returns its id or -1
int  UI_SliderRelease(void);           // ends an active grab, returns the slider id or -1
int  UI_SliderGrabbed(void);           // 1 while a slider is being dragged
void UI_SetMouse(float x, float y);
void UI_DrawAll(void);

#endif
