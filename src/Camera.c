#include "Camera.h"

const float toRadians = M_PI / 180.0;

GLfloat Yaw = -90.0f;
GLfloat Pitch = 0.0f;
GLfloat MovementSpeed = 10.1f;
GLfloat maxSpeed = 2000.0f;
GLfloat MouseSensitivity = 0.6f;
GLfloat Zoom = 45.0f;

float lastx, lasty;

mat4 tr, ry, rx, rxry;
vec3 Position = {0.0, 1.0, 0.0};
vec3 Up = {0.0, 0.0, 1.0};
vec3 Front = {0.0, 0.0, -1.0};
vec3 Right = {1.0, 0.0, 0.0};

mat4 getViewMatrix()
{
	rxry = multiplymat4(rx, ry);
	tr = translate(Position.x, Position.y, Position.z);
	return multiplymat4(rxry, tr);
}

mat4 getViewPosition()
{
	return tr;
}

mat4 getViewRotation()
{
	return rxry;
}

void updateCameraVectors()
{
	Front.x = rxry.m[2][0];
	Front.y = rxry.m[2][1];
	Front.z = rxry.m[2][2];
	Front = normalizevec3(Front);

	Right.x = rxry.m[0][0];
	Right.y = rxry.m[0][1];
	Right.z = rxry.m[0][2];
	Right = normalizevec3(Right);

	Up.x = rxry.m[1][0];
	Up.y = rxry.m[1][1];
	Up.z = rxry.m[1][2];
	Up = normalizevec3(Up);
}

void processKeyboard(enum Camera_Movement direction, GLfloat deltaTime, GLfloat deltaSpeed)
{
	if(deltaSpeed > maxSpeed)
		deltaSpeed = maxSpeed;
	GLfloat velocity = MovementSpeed * deltaTime + deltaSpeed;

    if (direction == FORWARD)
        Position = plusequalvec3(Position, scalarMultvec3(Front, velocity));
    if (direction == BACKWARD)
        Position = minusequalvec3(Position, scalarMultvec3(Front, velocity));
    if (direction == LEFT)
        Position = minusequalvec3(Position, scalarMultvec3(Right, velocity));
    if (direction == RIGHT)
        Position = plusequalvec3(Position, scalarMultvec3(Right, velocity));
}

int constrainPitch;
void processMouseMovement(GLfloat xpos, GLfloat ypos, int resetFlag)
{
	if(resetFlag) {
		lastx = xpos;
		lasty = ypos;
	}
	else {
		int diffx = xpos - lastx;
		int diffy = ypos - lasty;
		lastx = xpos;
		lasty = ypos;
		diffx *= MouseSensitivity;
		diffy *= MouseSensitivity;

		Yaw += diffx;
		Pitch += diffy;

		if(Pitch > 89.0)
			Pitch = 89.0f;
		if(Pitch < -89.0)
			Pitch = -89.0f;

		vec3 xAxis = {1.0, 0.0, 0.0};
		vec3 yAxis = {0.0, 1.0, 0.0};
		vec3 zAxis = {0.0, 0.0, 1.0};

		quaternion one = angleAxis(Pitch*toRadians, xAxis, zAxis);
		quaternion two = angleAxis(-Yaw*toRadians, yAxis, zAxis);
		//printf("quaternion: %f, %f, %f, %f\n", one.w, one.x, one.y, one.z);

		rx = quaternionToRotation(one);
		ry = quaternionToRotation(two);

		updateCameraVectors();
	}
}

float processMouseScroll(GLfloat yoffset, float zoom)
{
	if (zoom >= 1.0f && zoom <= 3.5f)
        zoom -= yoffset*0.1;
    if (zoom <= 1.0f)
        zoom = 1.0f;
    if (zoom >= 3.5f)
         zoom = 3.5f;
    return zoom;
}
