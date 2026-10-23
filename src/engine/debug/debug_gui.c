#include "engine/debug/debug_gui.h"
#include "engine/debug/profiler.h"

#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
/* The backend defaults to "#version 300 es" off Apple; desktop core GL
   only guarantees desktop GLSL. */
#define NK_SHADER_VERSION "#version 330 core\n"
#include "engine/debug/nk.h"
#include <nuklear/nuklear_glfw_gl3.h>

#define DEBUG_GUI_MAX_VERTEX_BUFFER  (512 * 1024)
#define DEBUG_GUI_MAX_ELEMENT_BUFFER (128 * 1024)

/* Sizes at 100% display scale; multiplied by the monitor content scale. */
#define DEBUG_GUI_TITLE   "Debug"
#define DEBUG_GUI_WIDTH   420.0f
#define DEBUG_GUI_HEIGHT  560.0f
#define DEBUG_GUI_MARGIN  24.0f
#define DEBUG_GUI_FONT_PX 18.0f
#define DEBUG_GUI_FONT_FILE "C:\\Windows\\Fonts\\segoeui.ttf"

#define DEBUG_GUI_MAX_PANELS 16

static struct nk_glfw nkGlfw;
static struct nk_context *ctx;
static int visible = 1;
static float uiScale = 1.0f; // monitor content scale (DPI), sampled at init
static float rowHeight;      // widget row height, from the font size

/* Screen area the panel covered last frame (header only while collapsed),
   window px y-down. The GLFW callbacks run between frames, so they test the
   live cursor against this. */
static struct nk_rect panelRect;
static int panelRectValid;

static float frameMs;

typedef struct Panel {
	const char *title;
	DebugGUI_PanelFn draw;
	DebugGUI_StartupFn startup;
} Panel;
static Panel panels[DEBUG_GUI_MAX_PANELS];
static int panelCount;

/* The panels' startup snapshots run on the first frame rather than at
   registration: the app sets some values (the wave parameters) after
   DebugGUI_Init, and nothing can be edited before the panel first draws. */
static int startupTaken;

void DebugGUI_Init(GLFWwindow *window)
{
	struct nk_font_atlas *atlas;
	struct nk_font *font;
	float sx, sy;

	glfwGetWindowContentScale(window, &sx, &sy);
	if (sy > 0.0f)
		uiScale = sy;
	float fontPx = DEBUG_GUI_FONT_PX * uiScale;
	rowHeight = fontPx * 1.4f;

	/* NK_GLFW3_DEFAULT: no callbacks installed; input.c forwards to us. */
	ctx = nk_glfw3_init(&nkGlfw, window, NK_GLFW3_DEFAULT);
	nk_glfw3_font_stash_begin(&nkGlfw, &atlas);
	/* Segoe UI stays legible when scaled up; the built-in ProggyClean is a
	   13px bitmap face that blurs. Fall back to it if the file is missing. */
	font = nk_font_atlas_add_from_file(atlas, DEBUG_GUI_FONT_FILE, fontPx, NULL);
	if (!font)
		font = nk_font_atlas_add_default(atlas, fontPx, NULL);
	nk_glfw3_font_stash_end(&nkGlfw);
	nk_style_set_font(ctx, &font->handle);
}

void DebugGUI_AddPanel(const char *title, DebugGUI_PanelFn draw, DebugGUI_StartupFn startup)
{
	if (panelCount == DEBUG_GUI_MAX_PANELS)
		return;
	panels[panelCount].title = title;
	panels[panelCount].draw = draw;
	panels[panelCount].startup = startup;
	panelCount++;
}

float DebugGUI_RowHeight(void)
{
	return rowHeight;
}

static const float sliderRatios[] = { 0.40f, 0.40f, 0.20f };

void DebugGUI_RowLayout(struct nk_context *c)
{
	nk_layout_row(c, NK_DYNAMIC, rowHeight, 3, sliderRatios);
}

void DebugGUI_Slider(struct nk_context *c, const char *name, float *value,
                     float lo, float hi, float step, const char *fmt)
{
	DebugGUI_RowLayout(c);
	nk_label(c, name, NK_TEXT_LEFT);
	nk_slider_float(c, lo, value, hi, step);
	nk_labelf(c, NK_TEXT_RIGHT, fmt, *value);
}

void DebugGUI_SliderInt(struct nk_context *c, const char *name, int *value, int lo, int hi)
{
	DebugGUI_RowLayout(c);
	nk_label(c, name, NK_TEXT_LEFT);
	nk_slider_int(c, lo, value, hi, 1);
	nk_labelf(c, NK_TEXT_RIGHT, "%d", *value);
}

void DebugGUI_Group(struct nk_context *c, const char *text)
{
	nk_layout_row_dynamic(c, rowHeight, 1);
	nk_label_colored(c, text, NK_TEXT_LEFT, nk_rgb(150, 170, 200));
}

/* Frame time and the profiler's per-pass times (profiler.c). */
static void performanceSection(void)
{
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_labelf(ctx, NK_TEXT_LEFT, "Frame: %.2f ms (%.0f fps)",
	          frameMs, frameMs > 0.0f ? 1000.0f / frameMs : 0.0f);
	/* Per pass: CPU (issue + quadtree walks) | GPU (timer queries). */
	nk_label(ctx, "Pass: CPU ms / GPU ms", NK_TEXT_LEFT);
	for (int i = 0; i < PROF_COUNT; i++)
		nk_labelf(ctx, NK_TEXT_LEFT, "  %s: %.2f / %.2f", Prof_Name((ProfSection)i),
		          Prof_CpuMs((ProfSection)i), Prof_GpuMs((ProfSection)i));
}

static void buildPanel(void)
{
	nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
	                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE;
	/* Only used when the window is first created; after that nuklear keeps
	   whatever position / size / collapse state the user left it in. */
	float w = DEBUG_GUI_WIDTH * uiScale, h = DEBUG_GUI_HEIGHT * uiScale;
	float margin = DEBUG_GUI_MARGIN * uiScale;
	struct nk_rect initial = nk_rect((float)nkGlfw.width - w - margin, margin, w, h);

	/* Hidden rather than skipped: a window not begun in a frame is freed,
	   which would lose its state across an F1 toggle. */
	nk_window_show(ctx, DEBUG_GUI_TITLE, visible ? NK_SHOWN : NK_HIDDEN);

	if (nk_begin(ctx, DEBUG_GUI_TITLE, initial, flags)) {
		if (nk_tree_push(ctx, NK_TREE_TAB, "Performance", NK_MAXIMIZED)) {
			performanceSection();
			nk_tree_pop(ctx);
		}
		/* The tree state is hashed from the call site, so one call site
		   needs a distinct id per tab. */
		for (int i = 0; i < panelCount; i++) {
			if (nk_tree_push_id(ctx, NK_TREE_TAB, panels[i].title, NK_MINIMIZED, i)) {
				panels[i].draw(ctx);
				nk_tree_pop(ctx);
			}
		}
	}

	panelRectValid = visible;
	panelRect = nk_window_get_bounds(ctx);
	if (nk_window_is_collapsed(ctx, DEBUG_GUI_TITLE)) {
		const struct nk_style_window_header *hd = &ctx->style.window.header;
		panelRect.h = ctx->style.font->height + 2.0f * (hd->padding.y + hd->label_padding.y);
	}
	nk_end(ctx);
}

void DebugGUI_Frame(void)
{
	if (!ctx)
		return;
	nk_glfw3_new_frame(&nkGlfw);

	if (!startupTaken) {
		for (int i = 0; i < panelCount; i++)
			if (panels[i].startup)
				panels[i].startup();
		startupTaken = 1;
	}

	/* Smoothed so the readout is legible rather than flickering. */
	float ms = ctx->delta_time_seconds * 1000.0f;
	frameMs = frameMs > 0.0f ? frameMs + (ms - frameMs) * 0.05f : ms;

	buildPanel();

	/* The backend leaves depth testing and culling off; restore them for the
	   next frame's scene passes. */
	GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
	nk_glfw3_render(&nkGlfw, NK_ANTI_ALIASING_ON,
	                DEBUG_GUI_MAX_VERTEX_BUFFER, DEBUG_GUI_MAX_ELEMENT_BUFFER);
	if (depthTest) glEnable(GL_DEPTH_TEST);
	if (cullFace) glEnable(GL_CULL_FACE);
}

void DebugGUI_Shutdown(void)
{
	if (!ctx)
		return;
	nk_glfw3_shutdown(&nkGlfw);
	ctx = NULL;
}

void DebugGUI_ToggleVisible(void)
{
	visible = !visible;
}

int DebugGUI_WantsMouse(double x, double y)
{
	if (!ctx || !panelRectValid)
		return 0;
	return x >= panelRect.x && x < panelRect.x + panelRect.w &&
	       y >= panelRect.y && y < panelRect.y + panelRect.h;
}

/* Mouse position / buttons are polled in nk_glfw3_new_frame; these feed the
   event-only parts (typed text, editing keys, wheel, double-click). */
void DebugGUI_KeyEvent(GLFWwindow *window, int key, int scancode, int action, int mods)
{
	if (ctx)
		nk_glfw3_key_callback(window, key, scancode, action, mods);
}

void DebugGUI_CharEvent(GLFWwindow *window, unsigned int codepoint)
{
	if (ctx)
		nk_glfw3_char_callback(window, codepoint);
}

void DebugGUI_ScrollEvent(GLFWwindow *window, double xoffset, double yoffset)
{
	if (ctx)
		nk_gflw3_scroll_callback(window, xoffset, yoffset);
}

void DebugGUI_MouseButtonEvent(GLFWwindow *window, int button, int action, int mods)
{
	if (ctx)
		nk_glfw3_mouse_button_callback(window, button, action, mods);
}
