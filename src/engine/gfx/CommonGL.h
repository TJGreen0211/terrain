#ifndef COMMONGL_H
#define COMMONGL_H
#include "engine/core/gl.h"

#include "engine/math/LinearAlg.h"
#include "engine/gfx/Textures.h"
#include "engine/gfx/LoadShader.h"

GLuint initFramebufferShader();
GLuint initFramebuffer(GLuint *textureID, float sizeX, float sizeY);

#endif
