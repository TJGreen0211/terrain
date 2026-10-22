#ifndef TEXTURES_H
#define TEXTURES_H
#include "engine/core/gl.h"

GLuint loadCubemap(char **faces);
GLuint loadTexture(char const * path, int alphaFlag);
GLuint loadSpriteTexture(char const * path);
GLuint generateTextureAttachment(int depth, int stencil, int sizex, int sizey);
GLuint generateDepthCubemap(int width, int height);

#endif
