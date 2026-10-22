#ifndef SKYBOX_H
#define SKYBOX_H

#include "engine/core/gl.h"
#include "engine/math/linalg.h"

// Compile the skybox program and upload its 36-triangle box. Call once after
// the GL context is ready (alongside Sun_Init).
void Skybox_Init(void);

// Build and return the cubemap from the six assets/textures/skybox3 faces.
GLuint Skybox_LoadCube(void);

// Draw the box as the far-plane background. Uses the camera's rotation-only
// matrix (no translation) so the box stays centred on the viewer and its
// rotation matches the world; draw it right after the clear, before the scene.
void Skybox_Draw(GLuint cubemap, mat4 projection, mat4 viewRotation);

#endif
