#include "Common.h"
#include "LoadShader.h"

//GLfloat deltaTime = 0.0f;
//GLfloat lastFrame = 0.0f;

void createShader(GLuint *shader, char *vert, char *frag)
{
	GLuint vertShader = LoadShader(vert, GL_VERTEX_SHADER);
    GLuint fragShader = LoadShader(frag, GL_FRAGMENT_SHADER);
    *shader = glCreateProgram();
    glAttachShader(*shader, vertShader);
    glAttachShader(*shader, fragShader);
    glLinkProgram(*shader);

    GLint linked;
    glGetProgramiv(*shader, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        char log[1024];
        glGetProgramInfoLog(*shader, sizeof(log), 0, log);
        fprintf(stderr, "Program link failed (%s + %s):\n %s\n", vert, frag, log);
    }
}

/*
GLfloat frameCount()
{
	GLfloat currentFrame = glfwGetTime();
    deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    return lastFrame;
}*/
