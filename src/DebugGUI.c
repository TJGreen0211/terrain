#define _CRT_SECURE_NO_WARNINGS // nuklear's NK_INCLUDE_STANDARD_IO uses fopen
#include "DebugGUI.h"
#include "Atmosphere.h"
#include "TerrainGen.h"
#include "TerrainDetail.h"
#include "Shadows.h"
#include "Water.h"
#include "Bodies.h"
#include "Profiler.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
/* The backend defaults to "#version 300 es" off Apple; desktop core GL
   only guarantees desktop GLSL. */
#define NK_SHADER_VERSION "#version 330 core\n"
#include <nuklear/nuklear.h>
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

/* Set by the regenerate buttons, consumed by the main loop next frame (the
   bake runs there, outside the GUI pass). */
static int regenerateRequested;

/* Startup values, restored by the reset buttons. Snapshotted on the first
   frame rather than at init: Main.c sets some (g_waveParams) after
   DebugGUI_Init, and nothing can be edited before the panel first draws. */
static int startupTaken;
static PlanetStyle styleAtStartup;
static TerrainBakeSettings bakeAtStartup;
static float wavesAtStartup[5];

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

	/* NK_GLFW3_DEFAULT: no callbacks installed; Input.c forwards to us. */
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

/* Empty sections show this until they are populated. */
static void placeholderRow(void)
{
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_label(ctx, "(nothing here yet)", NK_TEXT_LEFT);
}

/* One tunable per row: name | slider | value. Nuklear's property widgets
   print 2 decimals (NK_MAX_FLOAT_PRECISION, not overridable), too few for
   values like K_M, so the value is a label with its own format. */
static const float sliderRatios[] = { 0.40f, 0.40f, 0.20f };

static void sliderRow(const char *name, float *value, float lo, float hi, float step, const char *fmt)
{
	nk_layout_row(ctx, NK_DYNAMIC, rowHeight, 3, sliderRatios);
	nk_label(ctx, name, NK_TEXT_LEFT);
	nk_slider_float(ctx, lo, value, hi, step);
	nk_labelf(ctx, NK_TEXT_RIGHT, fmt, *value);
}

static void sliderRowInt(const char *name, int *value, int lo, int hi)
{
	nk_layout_row(ctx, NK_DYNAMIC, rowHeight, 3, sliderRatios);
	nk_label(ctx, name, NK_TEXT_LEFT);
	nk_slider_int(ctx, lo, value, hi, 1);
	nk_labelf(ctx, NK_TEXT_RIGHT, "%d", *value);
}

/* Muted sub-heading between groups of rows. */
static void groupLabel(const char *text)
{
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_label_colored(ctx, text, NK_TEXT_LEFT, nk_rgb(150, 170, 200));
}

static void atmosphereSection(void)
{
	atmosphereParams *a = &g_atmosphere;

	sliderRow("Shell height (m)", &a->shellHeight, 1.0e4f, 6.0e5f, 1.0e3f, "%.0f");
	sliderRow("Scale height (m)", &a->scaleHeight, 1.0e3f, 2.0e5f, 5.0e2f, "%.0f");
	sliderRow("C_R red", &a->C_R.x, 0.0f, 2.0f, 0.01f, "%.2f");
	sliderRow("C_R green", &a->C_R.y, 0.0f, 2.0f, 0.01f, "%.2f");
	sliderRow("C_R blue", &a->C_R.z, 0.0f, 2.0f, 0.01f, "%.2f");
	sliderRow("E (sun)", &a->E, 0.0f, 50.0f, 0.1f, "%.1f");
	sliderRow("K_R", &a->K_R, 0.0f, 1.0f, 0.001f, "%.3f");
	sliderRow("K_M", &a->K_M, 0.0f, 0.05f, 0.0001f, "%.4f");
	sliderRow("G_M", &a->G_M, -0.999f, -0.5f, 0.001f, "%.3f");

	nk_layout_row_dynamic(ctx, rowHeight, 1);
	if (nk_button_label(ctx, "Reset atmosphere"))
		*a = atmosphereDefaults();
}

/* Camera altitude above Earth's sea-level sphere, and the vertex precision
   comparison: the legacy path places quadtree vertices the old float way
   (unit sphere, then scaled), which jitters by ~0.5-1 m at Earth scale. */
static void cameraSection(void)
{
	const Body *earth = &g_bodies[BODY_EARTH];
	double alt = dlengthvec3(dsubvec3(getCameraPosition(), earth->position)) - earth->radius;

	nk_layout_row_dynamic(ctx, rowHeight, 1);
	if (fabs(alt) < 1e4)
		nk_labelf(ctx, NK_TEXT_LEFT, "Altitude: %.1f m", alt);
	else
		nk_labelf(ctx, NK_TEXT_LEFT, "Altitude: %.1f km", alt / 1000.0);
	nk_checkbox_label(ctx, "Legacy float vertex path", &g_qtLegacyPath);
	nk_checkbox_label(ctx, "Terrain detail noise", &g_terrainDetail);
	nk_checkbox_label(ctx, "Tint shadow cascades", &g_shadowDebug);
	nk_labelf(ctx, NK_TEXT_LEFT, "Shadow cascades: %d", g_cascades.count);
	for (int i = 0; i < g_cascades.count; i++)
		nk_labelf(ctx, NK_TEXT_LEFT, "  %d: radius %.0f m, texel %.2f m", i,
		          g_cascades.c[i].radius, g_cascades.c[i].texel);

	/* Totals over this frame's walks (shadow casters, moon, planet, water). */
	groupLabel("Terrain LOD (frame)");
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_labelf(ctx, NK_TEXT_LEFT, "Patches: %d   nodes: %d (%d built)", g_qtStats.leaves, g_qtStats.nodes, g_qtStats.built);
	nk_labelf(ctx, NK_TEXT_LEFT, "Deepest level: %d", g_qtStats.maxLevel);
	nk_labelf(ctx, NK_TEXT_LEFT, "Walks: %d   CPU: %.2f ms", g_qtStats.walks, g_qtStats.walkMs);
}

/* Live: g_waveParams is uploaded every frame. [0] (oceanOffset) is left
   out: the water shell must stay at sea level or it detaches from the
   coastline. */
static void waterSection(void)
{
	float *w = g_waveParams;

	sliderRow("Wave height", &w[1], 0.0f, 1.5f, 0.01f, "%.2f");
	sliderRow("Choppiness", &w[4], 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Normal strength", &w[2], 0.0f, 4.0f, 0.01f, "%.2f");
	sliderRow("Tiling", &w[3], 1.0f, 32.0f, 0.1f, "%.1f");

	nk_layout_row_dynamic(ctx, rowHeight, 1);
	if (nk_button_label(ctx, "Reset water"))
		memcpy(w, wavesAtStartup, sizeof wavesAtStartup);
}

/* Live: g_planetStyle is uploaded every frame. */
static void terrainSurfaceSection(void)
{
	PlanetStyle *s = &g_planetStyle;

	groupLabel("Relief (fraction of radius)");
	/* The quadtree's culling bounds follow these live (earthReliefBounds). */
	sliderRow("Land relief", &s->landRelief, 0.0f, 0.045f, 0.0005f, "%.4f");
	sliderRow("Ocean relief", &s->oceanRelief, 0.0f, 0.03f, 0.0005f, "%.4f");

	groupLabel("Climate");
	sliderRow("Temp offset", &s->tempOffset, -1.0f, 1.0f, 0.01f, "%+.2f");
	sliderRow("Lapse rate", &s->lapseRate, 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Snow temp", &s->snowTemp, -0.3f, 0.9f, 0.01f, "%.2f");
	sliderRow("Humidity", &s->humidity, 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Equator lush", &s->equatorLush, 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Hadley strength", &s->hadleyStrength, 0.0f, 1.0f, 0.01f, "%.2f");

	groupLabel("Features");
	sliderRow("River threshold", &s->riverThreshold, 0.0f, 1.0f, 0.01f, "%.2f");
	sliderRow("Rock slope", &s->rockSlope, 0.05f, 2.0f, 0.01f, "%.2f");

	nk_layout_row_dynamic(ctx, rowHeight, 1);
	if (nk_button_label(ctx, "Reset surface"))
		*s = styleAtStartup;
}

/* Bake inputs: edits do nothing until a regenerate. */
static void terrainGenerationSection(void)
{
	TerrainBakeSettings *b = &g_terrainBake;

	/* Seed as a typed / stepped property: unsigned in the struct, kept in
	   int range here. */
	int seed = b->seed > (unsigned int)INT_MAX ? INT_MAX : (int)b->seed;
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_property_int(ctx, "Seed", 0, &seed, INT_MAX, 1, 1.0f);
	b->seed = (unsigned int)seed;

	groupLabel("Plates");
	sliderRowInt("Plate count", &b->plateCount, 2, TERRAIN_PLATE_MAX);
	sliderRow("Continent area", &b->continentArea, 0.05f, 0.95f, 0.01f, "%.2f");

	groupLabel("Shape");
	sliderRow("Coast frequency", &b->coastFreq, 0.5f, 6.0f, 0.05f, "%.2f");
	sliderRow("Coast roughness", &b->coastAmt, 0.0f, 0.4f, 0.005f, "%.3f");
	sliderRow("Plains height", &b->plainsH, 0.0f, 0.3f, 0.005f, "%.3f");
	sliderRow("Hills", &b->hillAmt, 0.0f, 0.4f, 0.005f, "%.3f");
	sliderRow("Plateau height", &b->plateauH, 0.0f, 0.8f, 0.01f, "%.2f");
	sliderRow("Collision ranges", &b->collideH, 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Coastal ranges", &b->andesH, 0.0f, 3.0f, 0.01f, "%.2f");
	sliderRow("Old ranges", &b->oldRangeH, 0.0f, 1.0f, 0.01f, "%.2f");
	sliderRow("Ocean floor", &b->oceanBase, -1.5f, -0.1f, 0.01f, "%.2f");
	sliderRow("Mid-ocean ridges", &b->ridgeH, 0.0f, 1.0f, 0.01f, "%.2f");
	/* Upper bounds stay below the shader's fixed smoothstep tops. */
	sliderRow("Hotspot rarity", &b->hotspotLo, 0.2f, 0.79f, 0.01f, "%.2f");
	sliderRow("Microcontinent rarity", &b->fragLo, 0.2f, 0.7f, 0.01f, "%.2f");
	sliderRow("Rock detail", &b->detailAmt, 0.0f, 0.2f, 0.005f, "%.3f");

	groupLabel("Erosion");
	sliderRowInt("Thermal passes", &b->thermalPasses, 0, 20);
	sliderRow("Thermal talus", &b->thermalTalus, 0.001f, 0.03f, 0.0005f, "%.4f");
	sliderRow("Droplets / px (slow)", &b->dropletsPerPx, 0.0f, 1.5f, 0.01f, "%.2f");
	sliderRow("Erosion rate", &b->dropletErosion, 0.0f, 1.0f, 0.01f, "%.2f");
	sliderRow("Deposit rate", &b->dropletDeposit, 0.0f, 1.0f, 0.01f, "%.2f");
	sliderRow("Capacity", &b->dropletCapacity, 0.0f, 16.0f, 0.1f, "%.1f");
	sliderRow("Evaporation", &b->dropletEvap, 0.005f, 0.1f, 0.001f, "%.3f");
	sliderRow("Inertia", &b->dropletInertia, 0.0f, 0.9f, 0.01f, "%.2f");

	nk_layout_row_dynamic(ctx, rowHeight * 1.3f, 1);
	if (nk_button_label(ctx, "Regenerate planet"))
		regenerateRequested = 1;
	nk_layout_row_dynamic(ctx, rowHeight, 2);
	if (nk_button_label(ctx, "New seed")) {
		b->seed++;
		regenerateRequested = 1;
	}
	if (nk_button_label(ctx, "Reset settings")) {
		unsigned int keepSeed = b->seed; // keep the current planet's seed
		*b = bakeAtStartup;
		b->seed = keepSeed;
	}
	nk_layout_row_dynamic(ctx, rowHeight, 1);
	nk_label_colored(ctx, "Regenerating pauses the app for a few seconds.",
	                 NK_TEXT_LEFT, nk_rgb(150, 150, 150));
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
			nk_layout_row_dynamic(ctx, rowHeight, 1);
			nk_labelf(ctx, NK_TEXT_LEFT, "Frame: %.2f ms (%.0f fps)",
			          frameMs, frameMs > 0.0f ? 1000.0f / frameMs : 0.0f);
			/* Per pass: CPU (issue + quadtree walks) | GPU (timer queries). */
			nk_label(ctx, "Pass: CPU ms / GPU ms", NK_TEXT_LEFT);
			for (int i = 0; i < PROF_COUNT; i++)
				nk_labelf(ctx, NK_TEXT_LEFT, "  %s: %.2f / %.2f", Prof_Name((ProfSection)i),
				          Prof_CpuMs((ProfSection)i), Prof_GpuMs((ProfSection)i));
			nk_tree_pop(ctx);
		}
		if (nk_tree_push(ctx, NK_TREE_TAB, "Camera", NK_MINIMIZED)) {
			cameraSection();
			nk_tree_pop(ctx);
		}
		if (nk_tree_push(ctx, NK_TREE_TAB, "Terrain", NK_MINIMIZED)) {
			if (nk_tree_push(ctx, NK_TREE_NODE, "Surface (live)", NK_MAXIMIZED)) {
				terrainSurfaceSection();
				nk_tree_pop(ctx);
			}
			if (nk_tree_push(ctx, NK_TREE_NODE, "Generation (regenerate to apply)", NK_MINIMIZED)) {
				terrainGenerationSection();
				nk_tree_pop(ctx);
			}
			nk_tree_pop(ctx);
		}
		if (nk_tree_push(ctx, NK_TREE_TAB, "Atmosphere", NK_MINIMIZED)) {
			atmosphereSection();
			nk_tree_pop(ctx);
		}
		if (nk_tree_push(ctx, NK_TREE_TAB, "Water", NK_MINIMIZED)) {
			waterSection();
			nk_tree_pop(ctx);
		}
		if (nk_tree_push(ctx, NK_TREE_TAB, "Lighting", NK_MINIMIZED)) {
			placeholderRow();
			nk_tree_pop(ctx);
		}
	}

	panelRectValid = visible;
	panelRect = nk_window_get_bounds(ctx);
	if (nk_window_is_collapsed(ctx, DEBUG_GUI_TITLE)) {
		const struct nk_style_window_header *h = &ctx->style.window.header;
		panelRect.h = ctx->style.font->height + 2.0f * (h->padding.y + h->label_padding.y);
	}
	nk_end(ctx);
}

void DebugGUI_Frame(void)
{
	if (!ctx)
		return;
	nk_glfw3_new_frame(&nkGlfw);

	if (!startupTaken) {
		styleAtStartup = g_planetStyle;
		bakeAtStartup = g_terrainBake;
		memcpy(wavesAtStartup, g_waveParams, sizeof wavesAtStartup);
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

int DebugGUI_ConsumeRegenerate(void)
{
	int r = regenerateRequested;
	regenerateRequested = 0;
	return r;
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
