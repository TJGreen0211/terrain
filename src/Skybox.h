#ifndef SKYBOX_H
#define SKYBOX_H

#include <glad/glad.h>
#include "LinearAlg.h"

// Compile the skybox program and upload its 36-triangle box. Call once after
// the GL context is ready (alongside initSun).
void initSkybox(void);

// Build and return the cubemap from the six assets/textures/skybox3 faces.
GLuint loadSkyboxCube(void);

// Draw the box as the far-plane background. Uses the camera's rotation-only
// matrix (no translation) so the box stays centred on the viewer and its
// rotation matches the world; draw it right after the clear, before the scene.
void drawSkybox(GLuint cubemap, mat4 projection, mat4 viewRotation);

#endif
