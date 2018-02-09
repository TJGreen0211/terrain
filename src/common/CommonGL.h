#ifndef COMMONGL_H
#define COMMONGL_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <GLFW/glfw3.h>

#include "LinearAlg.h"
#include "Textures.h"
#include "LoadShader.h"

GLuint initFramebufferShader();
GLuint initFramebuffer(GLuint *textureID, float sizeX, float sizeY);

#endif
