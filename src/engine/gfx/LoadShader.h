#ifndef LOADSHADER_H
#define LOADSHADER_H
#include "engine/core/gl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* shaderName is relative to the shader root (engine/core/paths.h). */
GLuint Shader_Load(char *shaderName, int shaderType);
/* Same, with `defines` (e.g. "#define FOO\n") inserted after #version. */
GLuint Shader_LoadDefines(char *shaderName, int shaderType, const char *defines);

#endif