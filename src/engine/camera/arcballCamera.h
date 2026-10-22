#ifndef CAMERA_H
#define CAMERA_H
#include "engine/core/gl.h"

#include "engine/platform/Window.h"
#include "engine/math/LinearAlg.h"

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

/* Camera-relative rendering: getViewMatrix() is rotation only, and the camera
   sits at the origin of every space the GPU sees. getCameraPosition() is the
   absolute world camera in double: subtract it from world positions in double
   and downcast only the difference (see LinearAlg.h dvec3). */
void setCameraPivot(dvec3 pivot);   /* world position (kept relative to the focus) */
/* Focus body for this frame: the pivot rides with origin (world centre),
   the surface preset (G) stands on the body (radius) below the sun
   (sunDir: unit, body -> Sun). Call before using the camera each frame. */
void setCameraFocus(dvec3 origin, double radius, dvec3 sunDir);
/* After a focus switch: pivot on the body's centre, orbit at distance. */
void resetCameraForFocus(double distance);
dvec3 getCameraPosition(void);
mat4 getViewMatrix();
mat4 getViewRotation();

void updateCameraVectors();
void toggleSurfaceCamera(void);   // G: lock to a surface/tangent view
int isSurfaceCamera(void);
/* forward/right: summed WASD axes in [-1, 1] (W/D positive). */
void processKeyboard(float forward, float right, GLfloat deltaTime);
/* Arcball drag, driven by the real button press/release events. Cursor
   position and window size are both in GLFW screen coordinates. */
void arcballBegin(double xpos, double ypos, int width, int height);
void arcballDrag(double xpos, double ypos, int width, int height);
void arcballEnd(void);
int arcballDragging(void);
void processMouseScroll(float yoffset);

/* Benchmark path pose: orientation = pitch (world X) * yaw (world Y), orbit
   radius distMeters, drag state cleared. (See Bench.h scene format.) */
void benchSetOrbitPose(double yawDeg, double pitchDeg, float distMeters);

/* Benchmark subsolar pose: eye direction from the pivot given as a unit
   vector, so the pivot's local frame (not world +Y) can drive the camera. */
void benchSetOrbitEyeDir(double ux, double uy, double uz, float distMeters);

#endif
