#ifndef CAMERASLIDERS_H
#define CAMERASLIDERS_H

/* Camera sliders (bottom-left, blue = movement speed on top, orange =
   scroll speed below), ported from OpenGLSolar (CameraSliders.c). */
void CameraSliders_Setup(void);
/* Lays them out for a window of w x h; every frame, so they track resizes. */
void CameraSliders_Layout(int w, int h);

#endif
