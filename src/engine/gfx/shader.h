#ifndef SHADER_H
#define SHADER_H
#include "engine/core/gl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* shaderName is relative to the shader root (engine/core/paths.h). */
GLuint Shader_Load(char *shaderName, int shaderType);
/* Same, with `defines` (e.g. "#define FOO\n") inserted after #version. */
GLuint Shader_LoadDefines(char *shaderName, int shaderType, const char *defines);

void Shader_Create(GLuint *shader, char *vert, char *frag);
/* Variant: `defines` lines are inserted after #version in both stages. */
void Shader_CreateDefines(GLuint *shader, char *vert, char *frag, const char *defines);
//GLfloat frameCount();

#endif