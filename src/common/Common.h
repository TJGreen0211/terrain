#ifndef COMMON_H
#define COMMON_H
#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h> 
#include <GLFW/glfw3.h>

void createShader(GLuint *shader, char *vert, char *frag);
//GLfloat frameCount();

#endif
