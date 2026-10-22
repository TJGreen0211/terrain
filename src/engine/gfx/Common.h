#ifndef COMMON_H
#define COMMON_H
#include "engine/core/gl.h"

void Shader_Create(GLuint *shader, char *vert, char *frag);
/* Variant: `defines` lines are inserted after #version in both stages. */
void Shader_CreateDefines(GLuint *shader, char *vert, char *frag, const char *defines);
//GLfloat frameCount();

#endif
