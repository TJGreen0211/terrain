#include "engine/gfx/shader.h"
#include "engine/core/paths.h"
#include "engine/gfx/tone_map.h"

char *readShader(char *filename)
{
	FILE *fp;
	char *content = NULL;
	
	int count = 0;
	if(filename != NULL)
	{
		fp = fopen(filename, "rt");
		if(fp != NULL){
			fseek(fp, 0, SEEK_END);
			count = ftell(fp);
			rewind(fp);
			if (count > 0) {
            	content = (char *)malloc(sizeof(char) * (count+1));
            	count = fread(content,sizeof(char),count,fp);
            	content[count] = '\0';
        	}
			fclose(fp);
        }
	}
	return content;	
}

/* Expands `#include "file"` lines (nested up to MAX_INCLUDE_DEPTH), so shared
   GLSL lives in one file. "file" is looked up in the including file's
   directory first, then as a shader name ("world/clouds/cloudlayer.glsl",
   engine/core/paths.h), which is how a module includes another module's
   chunk. A #line after each insert keeps compile errors on the including
   file's numbering. */
#define MAX_INCLUDE_DEPTH 8
static char *expandIncludes(char *src, const char *shaderPath, int depth)
{
	if (src == NULL || depth >= MAX_INCLUDE_DEPTH || strstr(src, "#include") == NULL)
		return src;

	char dir[260] = "";
	const char *slash = strrchr(shaderPath, '/');
	if (slash != NULL && (size_t)(slash - shaderPath) + 1 < sizeof(dir)) {
		memcpy(dir, shaderPath, slash - shaderPath + 1);
		dir[slash - shaderPath + 1] = '\0';
	}

	size_t cap = strlen(src) + 1, len = 0;
	char *out = malloc(cap);
	int lineNo = 1;
	char *line = src;
	while (*line) {
		char *end = strchr(line, '\n');
		size_t lineLen = end ? (size_t)(end - line) + 1 : strlen(line);
		const char *p = line;
		while (*p == ' ' || *p == '\t') p++;

		char *insert = NULL;
		char directive[64] = "";
		if (strncmp(p, "#include", 8) == 0) {
			const char *q0 = strchr(p, '"');
			const char *q1 = q0 ? strchr(q0 + 1, '"') : NULL;
			if (q0 && q1 && q1 < line + lineLen) {
				char path[520], named[520] = "";
				int nameLen = (int)(q1 - q0 - 1);
				snprintf(path, sizeof(path), "%s%.*s", dir, nameLen, q0 + 1);
				char *text = readShader(path);
				if (text == NULL) {
					Paths_Shader(named, sizeof(named), "%.*s", nameLen, q0 + 1);
					text = readShader(named);
					if (text != NULL)
						snprintf(path, sizeof(path), "%s", named);
				}
				insert = expandIncludes(text, path, depth + 1);
				if (insert == NULL)
					fprintf(stderr, "Shader include not found: %s (tried %s and %s)\n",
					        shaderPath, path, named);
				snprintf(directive, sizeof(directive), "\n#line %d\n", lineNo + 1);
			}
		}

		const char *piece = insert ? insert : line;
		size_t pieceLen = insert ? strlen(insert) : lineLen;
		size_t need = len + pieceLen + strlen(directive) + 1;
		if (need > cap) {
			cap = need * 2;
			out = realloc(out, cap);
		}
		memcpy(out + len, piece, pieceLen);
		len += pieceLen;
		memcpy(out + len, directive, strlen(directive));
		len += strlen(directive);
		free(insert);

		line += lineLen;
		lineNo++;
	}
	out[len] = '\0';
	free(src);
	return out;
}

/* Inserts `defines` (whole lines) right after the #version line, so one
   source can build program variants. A #line keeps error numbering. */
static char *insertDefines(char *src, const char *defines)
{
	if (src == NULL || defines == NULL || defines[0] == '\0')
		return src;
	char *eol = strchr(src, '\n');
	size_t head = eol ? (size_t)(eol - src) + 1 : strlen(src);
	const char *lineDirective = "#line 2\n";
	size_t len = strlen(src) + strlen(defines) + strlen(lineDirective) + 2;
	char *out = malloc(len);
	memcpy(out, src, head);
	out[head] = '\0';
	if (!eol)
		strcat(out, "\n");
	strcat(out, defines);
	if (defines[strlen(defines) - 1] != '\n')
		strcat(out, "\n");
	strcat(out, lineDirective);
	strcat(out, src + head);
	free(src);
	return out;
}

GLuint Shader_Load(char *shaderPath, int shaderType)
{
	return Shader_LoadDefines(shaderPath, shaderType, NULL);
}

GLuint Shader_LoadDefines(char *shaderName, int shaderType, const char *defines)
{
	GLuint shader = glCreateShader(shaderType);

	char shaderPath[520];
	Paths_Shader(shaderPath, sizeof shaderPath, "%s", shaderName);
	char *shaderStr = insertDefines(expandIncludes(readShader(shaderPath), shaderPath, 0), defines);

	printf("Compiling %d, %s shader\n", shaderType, shaderPath);
	GLint length = strlen(shaderStr);
	glShaderSource(shader, 1, (const char **)&shaderStr, &length);
	glCompileShader(shader);
	
	//Check shader
	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if(success == GL_FALSE) {
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), 0, log);
		fprintf(stderr, "Shader compile failed (%s):\n %s \n", shaderPath, log);
		glDeleteShader(shader);
		return 1;
	}
	return shader;
	/*
	printf("Linking program\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    
    GLint programSuccess;
	glGetProgramiv(program, GL_LINK_STATUS, &programSuccess);
	if(programSuccess == GL_FALSE) {
		char log[1024];
		glGetProgramInfoLog(program, sizeof(log), 0, log);
		printf("shader link info: \n %s \n", log);
		glDeleteProgram(program);
		return 1;
	}
	
	//printf("%s", vertShaderStr);
	//printf("%s", fragShaderStr);
	return shader;*/
}

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
    /* Shared uniform blocks: GLSL 4.10 has no layout(binding = ...). */
    GLuint block = glGetUniformBlockIndex(*shader, "ToneMapBlock");
    if (block != GL_INVALID_INDEX)
        glUniformBlockBinding(*shader, block, TONEMAP_BINDING);
}

/*
GLfloat frameCount()
{
	GLfloat currentFrame = glfwGetTime();
    deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    return lastFrame;
}*/
