#ifndef CAMERA_H
#define CAMERA_H
#include "engine/core/gl.h"

#include "engine/platform/window.h"
#include "engine/math/linalg.h"

/* Scales, all in metres (and m/s). The app fills these from WorldConfig.h:
   the engine camera has no world constants of its own. */
typedef struct CameraConfig {
    double distanceMax;       /* farthest scroll-out orbit radius */
    double moveSpeed;         /* WASD speed, m/s */
    double scrollStep;        /* dolly per wheel notch */
    double spawnDistance;     /* initial pivot, in front of the focus body */
    double referenceRadius;   /* body radius the surface radii are given for */
    double surfaceMinRadius;  /* surface preset (G): radius range from the */
    double surfaceMaxRadius;  /*   body centre, for referenceRadius */
    double surfaceTiltDeg;    /* surface preset: view tilt below the horizon */
} CameraConfig;
/* Call once, before any other camera function. */
void Camera_Init(const CameraConfig *cfg);

/* WASD speed (m/s) and dolly per wheel notch (m); start at the config's. */
float Camera_MoveSpeed(void);
void Camera_SetMoveSpeed(float metresPerSecond);
float Camera_ScrollStep(void);
void Camera_SetScrollStep(float metres);
/* Factor on the WASD speed (1 = the slider's), e.g. slower near a surface
   the app knows about. */
void Camera_SetSpeedScale(double scale);
/* Keeps the camera at least minRadius from center (world, m), by moving
   the pivot straight out. No-op in the surface view. */
void Camera_KeepOutside(dvec3 center, double minRadius);

/* Camera-relative rendering: Camera_ViewMatrix() is rotation only, and the camera
   sits at the origin of every space the GPU sees. Camera_Position() is the
   absolute world camera in double: subtract it from world positions in double
   and downcast only the difference (see linalg.h dvec3). */
void Camera_SetPivot(dvec3 pivot);   /* world position (kept relative to the focus) */
/* Focus body for this frame: the pivot rides with origin (world centre),
   the surface preset (G) stands on the body (radius) below the sun
   (sunDir: unit, body -> Sun). Call before using the camera each frame. */
void Camera_SetFocus(dvec3 origin, double radius, dvec3 sunDir);
/* After a focus switch: pivot on the body's centre, orbit at distance. */
void Camera_ResetForFocus(double distance);
dvec3 Camera_Position(void);
mat4 Camera_ViewMatrix();
mat4 Camera_ViewRotation();

void Camera_UpdateVectors();
void Camera_ToggleSurface(void);   // G: lock to a surface/tangent view
int Camera_IsSurface(void);
/* forward/right/up: summed WASD + Space/LShift axes in [-1, 1] (W/D/Space
   positive), panning along the view. roll: Q/E axis in [-1, 1] (E positive,
   clockwise), turning about the view direction. */
void Camera_ProcessKeyboard(float forward, float right, float up, float roll, GLfloat deltaTime);
/* Arcball drag, driven by the real button press/release events. Cursor
   position and window size are both in GLFW screen coordinates. */
void Camera_ArcballBegin(double xpos, double ypos, int width, int height);
void Camera_ArcballDrag(double xpos, double ypos, int width, int height);
void Camera_ArcballEnd(void);
int Camera_ArcballDragging(void);
void Camera_ProcessScroll(float yoffset);

/* Benchmark path pose: orientation = pitch (world X) * yaw (world Y), orbit
   radius distMeters, drag state cleared. (See bench.h scene format.) */
void Camera_SetOrbitPose(double yawDeg, double pitchDeg, float distMeters);

/* Benchmark subsolar pose: eye direction from the pivot given as a unit
   vector, so the pivot's local frame (not world +Y) can drive the camera. */
void Camera_SetOrbitEyeDir(double ux, double uy, double uz, float distMeters);
/* The same with the screen's up taken from (upx, upy, upz) instead of
   world +Y (e.g. the local vertical, so the horizon is level). */
void Camera_SetOrbitEyeDirUp(double ux, double uy, double uz,
                             double upx, double upy, double upz, float distMeters);

#endif
