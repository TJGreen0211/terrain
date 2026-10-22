#include "engine/gfx/Common.h"
#include "engine/gfx/LoadShader.h"

//GLfloat deltaTime = 0.0f;
//GLfloat lastFrame = 0.0f;

void Shader_Create(GLuint *shader, char *vert, char *frag)
{
	Shader_CreateDefines(shader, vert, frag, NULL);
}

void Shader_CreateDefines(GLuint *shader, char *vert, char *frag, const char *defines)
{
	GLuint vertShader = Shader_LoadDefines(vert, GL_VERTEX_SHADER, defines);
    GLuint fragShader = Shader_LoadDefines(frag, GL_FRAGMENT_SHADER, defines);
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
