#ifndef TEXTURES_H
#define TEXTURES_H
#include "engine/core/gl.h"

GLuint Texture_LoadCubemap(char **faces);
GLuint Texture_Load(char const * path, int alphaFlag);
GLuint Texture_CreateAttachment(int depth, int stencil, int sizex, int sizey);

#endif
