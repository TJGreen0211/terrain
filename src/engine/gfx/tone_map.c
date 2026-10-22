#include "engine/gfx/tone_map.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"
#include <stdio.h>

static ToneMapParams params;
static GLuint ubo;

/* std140 layout of ToneMapBlock (tonemap.glsl): six vec4s. */
#define BLOCK_FLOATS 24

ToneMapParams *ToneMap_Params(void) { return &params; }

void ToneMap_ResetGrading(void)
{
	params.curve = TONEMAP_ACES;
	params.whitePoint = 8.0f;
	params.temperature = 0.0f;
	params.tint = 0.0f;
	params.saturation = 1.0f;
	params.contrast = 1.0f;
	for (int i = 0; i < 3; i++) {
		params.lift[i] = 0.0f;
		params.gamma[i] = 1.0f;
		params.gain[i] = 1.0f;
	}
}

void ToneMap_Init(void)
{
	params.exposure = 1.05f;
	params.dither = 1;
	params.debugView = TONEMAP_DEBUG_OFF;
	ToneMap_ResetGrading();

	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, BLOCK_FLOATS * sizeof(float), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	/* Filled now: init-time bakes include tonemap.glsl too. */
	ToneMap_Update();
}

void ToneMap_Update(void)
{
	/* White balance as linear per-channel scales: temperature trades red
	   against blue, tint green against magenta; normalised to keep the
	   luminance (Rec. 709 weights). */
	float wb[3] = {
		1.0f + 0.3f * params.temperature,
		1.0f - 0.3f * params.tint,
		1.0f - 0.3f * params.temperature
	};
	float luma = 0.2126f * wb[0] + 0.7152f * wb[1] + 0.0722f * wb[2];
	for (int i = 0; i < 3; i++)
		wb[i] /= luma;

	float block[BLOCK_FLOATS] = {
		/* toneMapSettings */ params.exposure, 0.0f, (float)params.curve, params.whitePoint,
		/* toneMapGrade */    params.saturation, params.contrast, params.dither ? 1.0f : 0.0f, (float)params.debugView,
		/* toneMapBalance */  wb[0], wb[1], wb[2], 0.0f,
		/* toneMapLift */     params.lift[0], params.lift[1], params.lift[2], 0.0f,
		/* toneMapGamma */    params.gamma[0], params.gamma[1], params.gamma[2], 0.0f,
		/* toneMapGain */     params.gain[0], params.gain[1], params.gain[2], 0.0f
	};
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof block, block);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, TONEMAP_BINDING, ubo);
}

static void rgbSliders(struct nk_context *ctx, const char *name, float *v, float lo, float hi)
{
	static const char *suffix[3] = { " R", " G", " B" };
	char label[64];
	for (int i = 0; i < 3; i++) {
		snprintf(label, sizeof label, "%s%s", name, suffix[i]);
		DebugGUI_Slider(ctx, label, &v[i], lo, hi, 0.005f, "%.3f");
	}
}

void ToneMap_DebugPanel(struct nk_context *ctx)
{
	static const char *curves[TONEMAP_CURVE_COUNT] = {
		"ACES fit", "Khronos PBR Neutral", "AgX", "Reinhard (extended)"
	};
	static const char *views[TONEMAP_DEBUG_COUNT] = {
		"Off", "Invalid values", "Luminance (EV)"
	};

	DebugGUI_Slider(ctx, "Exposure", &params.exposure, 0.1f, 4.0f, 0.01f, "%.2f");
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 2);
	nk_label(ctx, "Curve", NK_TEXT_LEFT);
	params.curve = nk_combo(ctx, curves, TONEMAP_CURVE_COUNT, params.curve,
	                        (int)DebugGUI_RowHeight(), nk_vec2(nk_widget_width(ctx), 160.0f));
	if (params.curve == TONEMAP_REINHARD)
		DebugGUI_Slider(ctx, "White point", &params.whitePoint, 1.0f, 32.0f, 0.1f, "%.1f");
	DebugGUI_Slider(ctx, "Temperature", &params.temperature, -1.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Tint", &params.tint, -1.0f, 1.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Saturation", &params.saturation, 0.0f, 2.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Contrast", &params.contrast, 0.5f, 1.5f, 0.01f, "%.2f");
	rgbSliders(ctx, "Lift", params.lift, -0.2f, 0.2f);
	rgbSliders(ctx, "Gamma", params.gamma, 0.5f, 2.0f);
	rgbSliders(ctx, "Gain", params.gain, 0.5f, 2.0f);
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	nk_checkbox_label(ctx, "Dither output", &params.dither);
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 2);
	nk_label(ctx, "Debug view", NK_TEXT_LEFT);
	params.debugView = nk_combo(ctx, views, TONEMAP_DEBUG_COUNT, params.debugView,
	                            (int)DebugGUI_RowHeight(), nk_vec2(nk_widget_width(ctx), 120.0f));
	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	if (nk_button_label(ctx, "Reset curve + grading"))
		ToneMap_ResetGrading();
}
