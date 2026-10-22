#ifndef LOADSHADER_H
#define LOADSHADER_H
#include "engine/core/gl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* shaderName is relative to the shader root (engine/core/paths.h). */
GLuint LoadShader(char *shaderName, int shaderType);
/* Same, with `defines` (e.g. "#define FOO\n") inserted after #version. */
GLuint LoadShaderDefines(char *shaderName, int shaderType, const char *defines);

#endif