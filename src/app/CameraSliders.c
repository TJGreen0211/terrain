#include "app/CameraSliders.h"
#include "engine/camera/arcballCamera.h"
#include "engine/debug/GUI.h"
#include "world/WorldConfig.h"
#include <math.h>

/* Camera sliders (bottom-left, blue = movement speed on top, orange =
   scroll speed below), ported from OpenGLSolar. Log-mapped so the low end
   stays usable: t in [0,1] -> min*(max/min)^t. The ranges (WorldConfig.h)
   keep the camera defaults at the same relative spot OpenGLSolar's do. */
#define UI_SLIDER_MOVEMENT 0
#define UI_SLIDER_SCROLL   1
#define UI_SLIDER_COUNT    2

#define CAMERA_MOVEMENT_MIN ((float)CAMERA_MOVE_SPEED_MIN)
#define CAMERA_MOVEMENT_MAX ((float)CAMERA_MOVE_SPEED_MAX)
#define CAMERA_SCROLL_MIN ((float)CAMERA_SCROLL_STEP_MIN)
#define CAMERA_SCROLL_MAX ((float)CAMERA_SCROLL_STEP_MAX)

static float cameraSliderValue(float minV, float maxV, float t)
{
	return minV * powf(maxV / minV, t);
}

static float cameraSliderInitT(float minV, float maxV, float value)
{
	return logf(value / minV) / logf(maxV / minV);
}

static void onCameraSliderChanged(int id, float t)
{
	if (id == UI_SLIDER_MOVEMENT)
		Camera_SetMoveSpeed(cameraSliderValue(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, t));
	else if (id == UI_SLIDER_SCROLL)
		Camera_SetScrollStep(cameraSliderValue(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, t));
}

/* Accent colours double as labels: blue = movement, orange = scroll. */
static const float sliderAccentMovement[3] = { 0.45f, 0.65f, 1.0f };
static const float sliderAccentScroll[3] = { 1.0f, 0.55f, 0.30f };

void CameraSliders_Setup(void)
{
	UI_SetSliderChangedFn(onCameraSliderChanged);
	/* t0 puts each handle at the current camera default. */
	UI_AddSlider(UI_SLIDER_MOVEMENT,
		cameraSliderInitT(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, Camera_MoveSpeed()),
		sliderAccentMovement);
	UI_AddSlider(UI_SLIDER_SCROLL,
		cameraSliderInitT(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, Camera_ScrollStep()),
		sliderAccentScroll);
}

void CameraSliders_Layout(int w, int h)
{
	UI_LayoutSliders(w, h, UI_SLIDER_COUNT);
}
