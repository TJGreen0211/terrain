#include "engine/debug/profiler.h"
#include "engine/core/gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Timer queries in flight per section: a result is read once it is
   PROF_LAG frames old, by which time the GPU has long finished it. */
#define PROF_LAG 4
#define PROF_SMOOTH 0.07f
#define PROF_MAX_SAMPLES 2048

static const char *names[PROF_COUNT] = {
	"Waves (FFT)", "Shadow map", "Moon", "Stand-ins", "Planet", "Water", "Clouds", "Rings", "Atmosphere", "Nebula", "Resolve", "UI"
};

static GLuint queries[PROF_LAG][PROF_COUNT];
static int issued[PROF_LAG][PROF_COUNT];
static int frame;
static float cpuMs[PROF_COUNT], gpuMs[PROF_COUNT];
static LARGE_INTEGER cpuStart[PROF_COUNT], freq;
static int ready;
static int disjointSupported;

/* Raw recording (benchmark): indexed by the frame the sample belongs to. */
static int recording;
static int sampleCount;
static float gpuSamples[PROF_COUNT][PROF_MAX_SAMPLES];
static float cpuSamples[PROF_COUNT][PROF_MAX_SAMPLES];
static float frameMs[PROF_MAX_SAMPLES];
static LARGE_INTEGER lastWavesBegin;
static int haveLastWaves;
static float pendingGpu[PROF_COUNT];   /* collected this end-of-frame */
static int pendingValid[PROF_COUNT];

void Prof_Init(void)
{
	glGenQueries(PROF_LAG * PROF_COUNT, &queries[0][0]);
	QueryPerformanceFrequency(&freq);
#ifdef GL_GPU_DISJOINT_EXT
	disjointSupported = GLAD_GL_EXT_disjoint_timer_query;
#endif
	ready = 1;
}

void Prof_EnableRawRecording(void)
{
	sampleCount = 0;
	recording = 1;
}

void Prof_Begin(ProfSection s)
{
	if (!ready)
		return;
	if (recording && s == PROF_WAVES && sampleCount < PROF_MAX_SAMPLES) {
		/* Frame interval, begin to begin: includes the swap wait of the
		   previous iteration, which is exactly the frame-to-frame cost. */
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		frameMs[sampleCount] = haveLastWaves
		    ? (float)(1000.0 * (double)(now.QuadPart - lastWavesBegin.QuadPart) / (double)freq.QuadPart)
		    : -1.0f;
		lastWavesBegin = now;
		haveLastWaves = 1;
	}
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
	if (recording && sampleCount < PROF_MAX_SAMPLES)
		cpuSamples[s][sampleCount] = ms;
}

void Prof_EndFrame(void)
{
	if (!ready)
		return;
	frame++;
	/* The slot about to be reused holds the oldest queries: collect them. */
	int slot = frame % PROF_LAG;
	for (int s = 0; s < PROF_COUNT; s++) {
		pendingValid[s] = 0;
		if (!issued[slot][s])
			continue;
		/* PROF_LAG frames old: normally long finished, so this rarely waits
		   (and a result is never dropped). */
		GLuint64 ns = 0;
		glGetQueryObjectui64v(queries[slot][s], GL_QUERY_RESULT, &ns);
		issued[slot][s] = 0;
		gpuMs[s] += ((float)(ns / 1.0e6) - gpuMs[s]) * PROF_SMOOTH;
		pendingGpu[s] = (float)(ns / 1.0e6);
		pendingValid[s] = 1;
#ifdef GL_GPU_DISJOINT_EXT
		if (disjointSupported) {
			GLint dis = 0;
			glGetIntegerv(GL_GPU_DISJOINT_EXT, &dis);
			if (dis)
				pendingValid[s] = 0;
		}
#endif
	}
	if (recording && sampleCount < PROF_MAX_SAMPLES) {
		for (int s = 0; s < PROF_COUNT; s++)
			gpuSamples[s][sampleCount] = pendingValid[s] ? pendingGpu[s] : -1.0f;
		sampleCount++;
	}
}

const char *Prof_Name(ProfSection s) { return names[s]; }
float Prof_CpuMs(ProfSection s) { return cpuMs[s]; }
float Prof_GpuMs(ProfSection s) { return gpuMs[s]; }

/* ---------------- stats JSON (benchmark harness) ---------------- */

static int cmpFloat(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/* Copy the finite, non-negative samples in [from,to) and print stats. */
static int printStats(FILE *f, const float *src, int from, int to, const char *indent)
{
	static float sorted[PROF_MAX_SAMPLES];
	int n = 0;
	for (int i = from; i < to; i++) {
		float v = src[i];
		if (v < 0.0f || v != v)
			continue;
		sorted[n++] = v;
	}
	if (!n) {
		fprintf(f, "%s\"count\":0\n", indent);
		return 0;
	}
	qsort(sorted, n, sizeof sorted[0], cmpFloat);
	int idx = n / 2;
	int i95 = (int)(0.95f * (n - 1) + 0.5f);
	int i99 = (int)(0.99f * (n - 1) + 0.5f);
	float mean = 0.0f;
	for (int i = 0; i < n; i++)
		mean += sorted[i];
	mean /= (float)n;
	fprintf(f, "%s\"count\":%d,\"minMs\":%.3f,\"medianMs\":%.3f,\"meanMs\":%.3f,\"p95Ms\":%.3f,\"p99Ms\":%.3f,\"maxMs\":%.3f\n",
	        indent, n, sorted[0], sorted[idx], mean, sorted[i95], sorted[i99], sorted[n - 1]);
	return n;
}

int Prof_WriteJson(const char *path, const char *scene, const char *extraJson)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	int from = sampleCount > 2 * PROF_WARMUP ? PROF_WARMUP : 0;
	int to = sampleCount;
	int n = 0, m;

	fprintf(f, "{\n  \"scene\": \"%s\",\n  \"samplesRecorded\": %d,\n  \"statsFromFrame\": %d,\n",
	        scene ? scene : "?", sampleCount, from);

	fprintf(f, "  \"frameMs\": {\n");
	m = printStats(f, frameMs, from, to, "    ");
	if (m) n = m;
	fprintf(f, "  },\n");

	fprintf(f, "  \"passes\": [\n");
	for (int s = 0; s < PROF_COUNT; s++) {
		fprintf(f, "    {\"name\": \"%s\"", names[s]);
		fprintf(f, ", \"gpu\": {\n");
		printStats(f, gpuSamples[s], from, to, "      ");
		fprintf(f, "    }, \"cpu\": {\n");
		printStats(f, cpuSamples[s], from, to, "      ");
		fprintf(f, "    } }%s\n", s + 1 < PROF_COUNT ? "," : "");
	}
	fprintf(f, "  ]");
	if (extraJson && extraJson[0])
		fprintf(f, ",\n  %s", extraJson);
	fprintf(f, "\n}\n");
	fclose(f);
	return n > 0;
}
