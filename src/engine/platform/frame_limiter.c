#include "engine/platform/frame_limiter.h"
#include "engine/core/gl.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"

/* Older SDKs lack the flag; Windows before 10 1803 rejects it, and the
   timer is then created without it (~1 ms resolution at best). */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sleep to this far short of the deadline, then spin: the timer can wake
   late by up to about this much. */
#define SPIN_MARGIN_S 0.0015

static const int fpsChoices[] = { 30, 60, 90, 120, 0 };
static const char *fpsLabels[] = { "30", "60", "90", "120", "Uncapped" };
#define FPS_CHOICE_COUNT (int)(sizeof fpsChoices / sizeof fpsChoices[0])

static FrameLimiterParams params = { FRAME_LIMITER_DEFAULT_FPS };
static HANDLE timer;
static LARGE_INTEGER freq;
static LONGLONG deadline;   /* QPC ticks; 0 = not started */
static int lastFps;

FrameLimiterParams *FrameLimiter_Params(void)
{
	return &params;
}

void FrameLimiter_Init(void)
{
	QueryPerformanceFrequency(&freq);
	timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                               TIMER_ALL_ACCESS);
	if (!timer)
		timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	/* The limiter paces frames; vsync would cap at the refresh rate instead. */
	glfwSwapInterval(0);
}

void FrameLimiter_Wait(void)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (params.fps <= 0) {
		deadline = 0;
		return;
	}
	LONGLONG period = freq.QuadPart / params.fps;
	/* First frame, a changed cap, or more than a period behind (a hitch,
	   a bake, the window dragged): restart the schedule from now. */
	if (deadline == 0 || params.fps != lastFps || now.QuadPart - deadline > period) {
		deadline = now.QuadPart + period;
		lastFps = params.fps;
	}

	double remaining = (double)(deadline - now.QuadPart) / (double)freq.QuadPart;
	if (timer && remaining > SPIN_MARGIN_S) {
		/* Relative due time, in negative 100 ns units. */
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)((remaining - SPIN_MARGIN_S) * 1e7);
		if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
			WaitForSingleObject(timer, INFINITE);
	}
	do {
		YieldProcessor();
		QueryPerformanceCounter(&now);
	} while (now.QuadPart < deadline);

	deadline += period;
}

void FrameLimiter_Shutdown(void)
{
	if (timer)
		CloseHandle(timer);
	timer = NULL;
}

void FrameLimiter_DebugPanel(struct nk_context *ctx)
{
	int current = FPS_CHOICE_COUNT - 1;
	for (int i = 0; i < FPS_CHOICE_COUNT; i++)
		if (fpsChoices[i] == params.fps)
			current = i;

	DebugGUI_RowLayout(ctx);
	nk_label(ctx, "FPS cap", NK_TEXT_LEFT);
	float h = DebugGUI_RowHeight();
	current = nk_combo(ctx, fpsLabels, FPS_CHOICE_COUNT, current, (int)h,
	                   nk_vec2(nk_widget_width(ctx), h * (FPS_CHOICE_COUNT + 1)));
	params.fps = fpsChoices[current];
	nk_spacing(ctx, 1);
}
