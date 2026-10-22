#include "world/terrain/TerrainPanel.h"
#include "world/terrain/TerrainGen.h"
#include "world/terrain/TerrainDetail.h"
#include "world/terrain/QuadTree.h"
#include "world/WorldConfig.h"
#include "engine/debug/DebugGUI.h"
#include "engine/debug/nk.h"
#include <limits.h>

/* Set by the regenerate buttons, consumed by the app next frame (the bake
   runs there, outside the GUI pass). */
static int regenerateRequested;

/* Startup values, restored by the reset buttons (Terrain_DebugPanelStartup). */
static PlanetStyle styleAtStartup;
static TerrainBakeSettings bakeAtStartup;

void Terrain_DebugPanelStartup(void)
{
	styleAtStartup = *TerrainGen_Style();
	bakeAtStartup = *TerrainGen_BakeSettings();
}

int Terrain_ConsumeRegenerate(void)
{
	int r = regenerateRequested;
	regenerateRequested = 0;
	return r;
}

/* Live: TerrainGen_Style() is uploaded every frame. */
static void surfaceSection(struct nk_context *ctx)
{
	PlanetStyle *s = TerrainGen_Style();

	DebugGUI_Group(ctx, "Relief (fraction of radius)");
	/* The quadtree's culling bounds follow these live (earthReliefBounds). */
	DebugGUI_Slider(ctx, "Land relief", &s->landRelief, 0.0f, 0.045f, 0.0005f, "%.4f");
	DebugGUI_Slider(ctx, "Ocean relief", &s->oceanRelief, 0.0f, 0.03f, 0.0005f, "%.4f");

	DebugGUI_Group(ctx, "Climate");
	DebugGUI_Slider(ctx, "Temp offset", &s->tempOffset, -1.0f, 1.0f, 0.01f, "%+.2f");
	DebugGUI_Slider(ctx, "Lapse rate", &s->lapseRate, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Snow temp", &s->snowTemp, -0.3f, 0.9f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Humidity", &s->humidity, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Equator lush", &s->equatorLush, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Hadley strength", &s->hadleyStrength, 0.0f, 1.0f, 0.01f, "%.2f");

	DebugGUI_Group(ctx, "Features");
	DebugGUI_Slider(ctx, "River threshold", &s->riverThreshold, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Rock slope", &s->rockSlope, 0.05f, 2.0f, 0.01f, "%.2f");

	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	if (nk_button_label(ctx, "Reset surface"))
		*s = styleAtStartup;
}

/* Bake inputs: edits do nothing until a regenerate. */
static void generationSection(struct nk_context *ctx)
{
	TerrainBakeSettings *b = TerrainGen_BakeSettings();

	/* Seed as a typed / stepped property: unsigned in the struct, kept in
	   int range here. */
	int seed = b->seed > (unsigned int)INT_MAX ? INT_MAX : (int)b->seed;
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_property_int(ctx, "Seed", 0, &seed, INT_MAX, 1, 1.0f);
	b->seed = (unsigned int)seed;

	DebugGUI_Group(ctx, "Plates");
	DebugGUI_SliderInt(ctx, "Plate count", &b->plateCount, 2, TERRAIN_PLATE_MAX);
	DebugGUI_Slider(ctx, "Continent area", &b->continentArea, 0.05f, 0.95f, 0.01f, "%.2f");

	DebugGUI_Group(ctx, "Shape");
	DebugGUI_Slider(ctx, "Coast frequency", &b->coastFreq, 0.5f, 6.0f, 0.05f, "%.2f");
	DebugGUI_Slider(ctx, "Coast roughness", &b->coastAmt, 0.0f, 0.4f, 0.005f, "%.3f");
	DebugGUI_Slider(ctx, "Plains height", &b->plainsH, 0.0f, 0.3f, 0.005f, "%.3f");
	DebugGUI_Slider(ctx, "Hills", &b->hillAmt, 0.0f, 0.4f, 0.005f, "%.3f");
	DebugGUI_Slider(ctx, "Plateau height", &b->plateauH, 0.0f, 0.8f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Collision ranges", &b->collideH, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Coastal ranges", &b->andesH, 0.0f, 3.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Old ranges", &b->oldRangeH, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Ocean floor", &b->oceanBase, -1.5f, -0.1f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Mid-ocean ridges", &b->ridgeH, 0.0f, 1.0f, 0.01f, "%.2f");
	/* Upper bounds stay below the shader's fixed smoothstep tops. */
	DebugGUI_Slider(ctx, "Hotspot rarity", &b->hotspotLo, 0.2f, 0.79f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Microcontinent rarity", &b->fragLo, 0.2f, 0.7f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Rock detail", &b->detailAmt, 0.0f, 0.2f, 0.005f, "%.3f");

	DebugGUI_Group(ctx, "Erosion");
	DebugGUI_SliderInt(ctx, "Thermal passes", &b->thermalPasses, 0, 20);
	DebugGUI_Slider(ctx, "Thermal talus", &b->thermalTalus, 0.001f, 0.03f, 0.0005f, "%.4f");
	DebugGUI_Slider(ctx, "Droplets / px (slow)", &b->dropletsPerPx, 0.0f, 1.5f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Erosion rate", &b->dropletErosion, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Deposit rate", &b->dropletDeposit, 0.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Capacity", &b->dropletCapacity, 0.0f, 16.0f, 0.1f, "%.1f");
	DebugGUI_Slider(ctx, "Evaporation", &b->dropletEvap, 0.005f, 0.1f, 0.001f, "%.3f");
	DebugGUI_Slider(ctx, "Inertia", &b->dropletInertia, 0.0f, 0.9f, 0.01f, "%.2f");

	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight() * 1.3f, 1);
	if (nk_button_label(ctx, "Regenerate planet"))
		regenerateRequested = 1;
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 2);
	if (nk_button_label(ctx, "New seed")) {
		b->seed++;
		regenerateRequested = 1;
	}
	if (nk_button_label(ctx, "Reset settings")) {
		unsigned int keepSeed = b->seed; // keep the current planet's seed
		*b = bakeAtStartup;
		b->seed = keepSeed;
	}
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_label_colored(ctx, "Regenerating pauses the app for a few seconds.",
	                 NK_TEXT_LEFT, nk_rgb(150, 150, 150));
}

/* Vertex precision comparison: the legacy path places quadtree vertices
   the old float way (unit sphere, then scaled), which jitters by ~0.5-1 m
   at Earth scale. Then the detail knobs (TerrainDetail.c) and this frame's
   LOD totals. */
static void viewSection(struct nk_context *ctx)
{
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Legacy float vertex path", &QuadTree_Params()->legacyPath);
	nk_checkbox_label(ctx, "Terrain detail noise", &TerrainDetail_Params()->enabled);
	DebugGUI_Slider(ctx, "Detail roughness", &TerrainDetail_Params()->roughness, 0.0f, 3.0f, 0.05f, "%.2f");
	DebugGUI_Slider(ctx, "Terrain AO", &TerrainDetail_Params()->ao, 0.0f, 1.0f, 0.05f, "%.2f");
	DebugGUI_Slider(ctx, "Ground textures", &TerrainDetail_Params()->texStrength, 0.0f, 1.0f, 0.05f, "%.2f");

	/* Totals over this frame's walks (shadow casters, moon, planet, water). */
	DebugGUI_Group(ctx, "Terrain LOD (frame)");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_labelf(ctx, NK_TEXT_LEFT, "Patches: %d   nodes: %d (%d built)", QuadTree_FrameStats()->leaves, QuadTree_FrameStats()->nodes, QuadTree_FrameStats()->built);
	nk_labelf(ctx, NK_TEXT_LEFT, "Deepest level: %d", QuadTree_FrameStats()->maxLevel);
	nk_labelf(ctx, NK_TEXT_LEFT, "Walks: %d   CPU: %.2f ms", QuadTree_FrameStats()->walks, QuadTree_FrameStats()->walkMs);
}

void Terrain_DebugPanel(struct nk_context *ctx)
{
	if (nk_tree_push(ctx, NK_TREE_NODE, "Surface (live)", NK_MAXIMIZED)) {
		surfaceSection(ctx);
		nk_tree_pop(ctx);
	}
	if (nk_tree_push(ctx, NK_TREE_NODE, "Generation (regenerate to apply)", NK_MINIMIZED)) {
		generationSection(ctx);
		nk_tree_pop(ctx);
	}
	if (nk_tree_push(ctx, NK_TREE_NODE, "Detail and LOD", NK_MINIMIZED)) {
		viewSection(ctx);
		nk_tree_pop(ctx);
	}
}
