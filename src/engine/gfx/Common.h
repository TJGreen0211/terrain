#ifndef COMMON_H
#define COMMON_H
#include "engine/core/gl.h"

void createShader(GLuint *shader, char *vert, char *frag);
/* Variant: `defines` lines are inserted after #version in both stages. */
void createShaderDefines(GLuint *shader, char *vert, char *frag, const char *defines);
//GLfloat frameCount();

#endif
