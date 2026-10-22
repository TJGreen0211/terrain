#ifndef LOADSHADER_H
#define LOADSHADER_H
#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h> 
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* shaderName is relative to the shader root (engine/core/paths.h). */
GLuint LoadShader(char *shaderName, int shaderType);
/* Same, with `defines` (e.g. "#define FOO\n") inserted after #version. */
GLuint LoadShaderDefines(char *shaderName, int shaderType, const char *defines);

#endif