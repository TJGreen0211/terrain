#include "Profiler.h"
#include <windows.h>
#include <glad/glad.h>

/* Timer queries in flight per section: a result is read once it is
   PROF_LAG frames old, by which time the GPU has long finished it. */
#define PROF_LAG 4
#define PROF_SMOOTH 0.07f

static const char *names[PROF_COUNT] = {
	"Waves (FFT)", "Shadow map", "Moon", "Planet", "Water", "Rings", "Atmosphere", "Resolve + UI"
};

static GLuint queries[PROF_LAG][PROF_COUNT];
static int issued[PROF_LAG][PROF_COUNT];
static int frame;
static float cpuMs[PROF_COUNT], gpuMs[PROF_COUNT];
static LARGE_INTEGER cpuStart[PROF_COUNT], freq;
static int ready;

void Prof_Init(void)
{
	glGenQueries(PROF_LAG * PROF_COUNT, &queries[0][0]);
	QueryPerformanceFrequency(&freq);
	ready = 1;
}

void Prof_Begin(ProfSection s)
{
	if (!ready)
		return;
	QueryPerformanceCounter(&cpuStart[s]);
	glBeginQuery(GL_TIME_ELAPSED, queries[frame % PROF_LAG][s]);
}

void Prof_End(ProfSection s)
{
	if (!ready)
		return;
	glEndQuery(GL_TIME_ELAPSED);
	issued[frame % PROF_LAG][s] = 1;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	float ms = (float)(1000.0 * (double)(now.QuadPart - cpuStart[s].QuadPart) / (double)freq.QuadPart);
	cpuMs[s] += (ms - cpuMs[s]) * PROF_SMOOTH;
}

void Prof_EndFrame(void)
{
	if (!ready)
		return;
	frame++;
	/* The slot about to be reused holds the oldest queries: collect them. */
	int slot = frame % PROF_LAG;
	for (int s = 0; s < PROF_COUNT; s++) {
		if (!issued[slot][s])
			continue;
		/* PROF_LAG frames old: normally long finished, so this rarely waits
		   (and a result is never dropped). */
		GLuint64 ns = 0;
		glGetQueryObjectui64v(queries[slot][s], GL_QUERY_RESULT, &ns);
		gpuMs[s] += ((float)(ns / 1.0e6) - gpuMs[s]) * PROF_SMOOTH;
		issued[slot][s] = 0;
	}
}

const char *Prof_Name(ProfSection s) { return names[s]; }
float Prof_CpuMs(ProfSection s) { return cpuMs[s]; }
float Prof_GpuMs(ProfSection s) { return gpuMs[s]; }
