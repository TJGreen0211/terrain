#include "GUI.h"

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

static GLuint UIShader;
static GLint uiLRect, uiLColor, uiLHasTex, uiLTint, uiLAlpha, uiLViewport, uiLTex;
static GLint uiLUVOff, uiLUVScale, uiLTintTex;
static GLuint UIvao, UIvbo;
static uiSlider sliders[UI_MAX_SLIDERS];
static int sliderCount = 0;
static int activeSlider = -1;
static uiSliderChangedFn sliderChanged = NULL;
static int uiReady = 0;

void attachGUIShaders()
{
	createShader(&UIShader, "shaders/UI.vert",
		"shaders/UI.frag");
	uiLRect = glGetUniformLocation(UIShader, "uRect");
	uiLColor = glGetUniformLocation(UIShader, "uColor");
	uiLHasTex = glGetUniformLocation(UIShader, "uHasTex");
	uiLTint = glGetUniformLocation(UIShader, "uTint");
	uiLAlpha = glGetUniformLocation(UIShader, "uAlpha");
	uiLViewport = glGetUniformLocation(UIShader, "uViewport");
	uiLTex = glGetUniformLocation(UIShader, "uTex");
	uiLUVOff = glGetUniformLocation(UIShader, "uUVOff");
	uiLUVScale = glGetUniformLocation(UIShader, "uUVScale");
	uiLTintTex = glGetUniformLocation(UIShader, "uTintTex");
}

int uiInit(void)
{
	if (uiReady)
		return 1;

	static const float quad[] = {
		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,

		-1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
		 1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,  0.0f, 0.0f
	};

	glGenVertexArrays(1, &UIvao);
	glBindVertexArray(UIvao);
	glGenBuffers(1, &UIvbo);
	glBindBuffer(GL_ARRAY_BUFFER, UIvbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

	GLint vPosition = glGetAttribLocation(UIShader, "vPosition");
	glVertexAttribPointer(vPosition, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), BUFFER_OFFSET(0));
	glEnableVertexAttribArray(vPosition);

	GLint texCoord = glGetAttribLocation(UIShader, "texCoord");
	glVertexAttribPointer(texCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), BUFFER_OFFSET(3 * sizeof(GLfloat)));
	glEnableVertexAttribArray(texCoord);

	glBindVertexArray(0);
	uiReady = 1;
	return 1;
}

static int pointInRect(float x, float y, const float rect[4])
{
	return x >= rect[0] && x < rect[0] + rect[2] &&
	       y >= rect[1] && y < rect[1] + rect[3];
}

int uiAddSlider(int id, float t0, const float accent[3])
{
	if (!uiReady || sliderCount >= UI_MAX_SLIDERS)
		return -1;

	uiSlider *s = &sliders[sliderCount];
	s->id = id;
	if (accent)
	{
		s->accent[0] = accent[0];
		s->accent[1] = accent[1];
		s->accent[2] = accent[2];
	}
	else
	{
		s->accent[0] = 0.45f;
		s->accent[1] = 0.65f;
		s->accent[2] = 1.0f;
	}
	s->t = t0 < 0.0f ? 0.0f : (t0 > 1.0f ? 1.0f : t0);
	s->rect[0] = s->rect[1] = s->rect[2] = s->rect[3] = 0.0f;
	s->hovered = 0;
	s->active = 0;
	sliderCount++;
	/* Sync the consumer with the starting position. */
	if (sliderChanged)
		sliderChanged(id, s->t);
	return sliderCount - 1;
}

int uiSliderCount(void)
{
	return sliderCount;
}

/* Stacked horizontal sliders anchored at the bottom-left corner. */
void uiLayoutSliders(int winW, int winH, int count)
{
	if (!uiReady || count <= 0)
		return;
	if (count > sliderCount)
		count = sliderCount;

	float w = (float)winW * 0.18f;
	if (w > 240.0f)
		w = 240.0f;
	if (w < 120.0f)
		w = 120.0f;
	float h = 36.0f;
	float gap = 12.0f;
	float margin = 24.0f;

	float x = margin;
	float totalH = (float)count * h + (float)(count - 1) * gap;
	float y = (float)winH - margin - totalH;
	if (y < 0.0f)
		y = 0.0f;

	for (int i = 0; i < count; i++)
	{
		sliders[i].rect[0] = x;
		sliders[i].rect[1] = y;
		sliders[i].rect[2] = w;
		sliders[i].rect[3] = h;
		y += h + gap;
	}
}

void uiSliderSetChangedFn(uiSliderChangedFn fn)
{
	sliderChanged = fn;
}

static void sliderSetT(uiSlider *s, float x)
{
	float t = (x - s->rect[0]) / s->rect[2];
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (t != s->t)
	{
		s->t = t;
		if (sliderChanged)
			sliderChanged(s->id, t);
	}
}

int uiSliderPress(float x, float y)
{
	if (!uiReady)
		return -1;
	for (int i = 0; i < sliderCount; i++)
		if (pointInRect(x, y, sliders[i].rect))
		{
			activeSlider = i;
			sliders[i].active = 1;
			sliderSetT(&sliders[i], x);
			return sliders[i].id;
		}
	return -1;
}

int uiSliderRelease(void)
{
	if (activeSlider < 0)
		return -1;
	int id = sliders[activeSlider].id;
	sliders[activeSlider].active = 0;
	activeSlider = -1;
	return id;
}

int uiSliderGrabbed(void)
{
	return activeSlider >= 0;
}

void uiSetMouse(float x, float y)
{
	if (!uiReady)
		return;
	for (int i = 0; i < sliderCount; i++)
	{
		sliders[i].hovered = pointInRect(x, y, sliders[i].rect);
		if (activeSlider == i)
			sliderSetT(&sliders[i], x);
	}
}

/* Flat (untextured) UI quad. */
static void uiDrawFlat(float x, float y, float w, float h,
	float r, float g, float b, float a, float tint)
{
	glUniform4f(uiLRect, x, y, w, h);
	glUniform4f(uiLColor, r, g, b, a);
	glUniform1f(uiLHasTex, 0.0f);
	glUniform1f(uiLTint, tint);
	glUniform1f(uiLAlpha, 1.0f);
	glUniform1f(uiLTintTex, 0.0f);
	glUniform2f(uiLUVOff, 0.0f, 0.0f);
	glUniform2f(uiLUVScale, 1.0f, 1.0f);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Track + handle per slider, drawn last in the frame with standard alpha
   blending and no depth test. The scene viewport here is the monitor video
   mode size, not the window, so the UI pass draws into the full framebuffer
   and maps window pixels through the window size: the rects then line up
   with glfwGetCursorPos regardless of the scene viewport. */
void uiDrawAll(void)
{
	if (!uiReady || sliderCount == 0)
		return;

	GLFWwindow *window = glfwGetCurrentContext();
	int winW, winH, fbW, fbH;
	glfwGetWindowSize(window, &winW, &winH);
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (winW <= 0 || winH <= 0)
		return;   // minimized

	int viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glViewport(0, 0, fbW, fbH);

	glUseProgram(UIShader);
	glUniform2f(uiLViewport, (float)winW, (float)winH);
	glUniform1i(uiLTex, 0);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);

	glBindVertexArray(UIvao);

	for (int i = 0; i < sliderCount; i++)
	{
		uiSlider *s = &sliders[i];
		float x = s->rect[0], y = s->rect[1], w = s->rect[2], h = s->rect[3];
		float tint = s->active ? 0.5f : (s->hovered ? 0.25f : 0.0f);

		uiDrawFlat(x, y, w, h, 0.04f, 0.05f, 0.09f, 0.6f, 0.0f);

		/* Track, filled portion left of the handle, then the handle — fill
		   and handle in the slider's accent colour so each slider reads as
		   its own thing. */
		float trackX = x + 8.0f;
		float trackW = w - 16.0f;
		float trackY = y + h * 0.5f - 2.0f;
		uiDrawFlat(trackX, trackY, trackW, 4.0f, 0.22f, 0.25f, 0.32f, 0.9f, 0.0f);
		if (s->t > 0.0f)
			uiDrawFlat(trackX, trackY, trackW * s->t, 4.0f,
				s->accent[0], s->accent[1], s->accent[2], 0.9f, 0.0f);
		uiDrawFlat(trackX + trackW * s->t - 7.0f, y + h * 0.5f - 7.0f,
			14.0f, 14.0f, s->accent[0], s->accent[1], s->accent[2], 1.0f, tint);
	}

	glBindVertexArray(0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

void uiShutdown(void)
{
	if (!uiReady)
		return;
	uiReady = 0;
	sliderCount = 0;
	activeSlider = -1;
	glDeleteVertexArrays(1, &UIvao);
	glDeleteBuffers(1, &UIvbo);
	glDeleteProgram(UIShader);
}
