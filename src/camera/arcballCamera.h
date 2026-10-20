#ifndef CAMERA_H
#define CAMERA_H
#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h> 
#include <GLFW/glfw3.h>

#include "Window.h"
#include "LinearAlg.h"

enum Camera_Movement {
	FORWARD,
	BACKWARD,
	LEFT,
	RIGHT
};

extern float CameraDistance;
extern const float CameraDistanceMax;
extern GLfloat MovementSpeed;
extern GLfloat CameraScrollSpeed;

/* Camera-relative rendering: getViewMatrix() is rotation only, and the camera
   sits at the origin of every space the GPU sees. getCameraPosition() is the
   absolute world camera in double: subtract it from world positions in double
   and downcast only the difference (see LinearAlg.h dvec3). */
void setCameraPivot(dvec3 pivot);
dvec3 getCameraPosition(void);
mat4 getViewMatrix();
mat4 getViewRotation();

void updateCameraVectors();
void toggleSurfaceCamera(void);   // G: lock to a surface/tangent view
int isSurfaceCamera(void);
void processKeyboard(enum Camera_Movement direction, GLfloat deltaTime , GLfloat deltaSpeed);
void processMouseMovement(float xpos, float ypos, int resetFlag);
float processMouseScroll(float yoffset);

#endif
