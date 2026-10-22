#ifndef CAMERA_H
#define CAMERA_H
#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h> 
#include <GLFW/glfw3.h>

#include "engine/platform/Window.h"
#include "engine/math/LinearAlg.h"

extern float CameraDistance;
extern const float CameraDistanceMax;
extern GLfloat MovementSpeed;
extern GLfloat CameraScrollSpeed;

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
