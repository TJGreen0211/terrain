#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include "engine/core/gl.h"

#include "engine/math/linalg.h"
#include "engine/gfx/texture.h"
#include "engine/gfx/shader.h"

GLuint Framebuffer_Create(GLuint *textureID, float sizeX, float sizeY);

#endif
