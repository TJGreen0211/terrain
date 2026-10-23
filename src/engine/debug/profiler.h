#ifndef PROFILER_H
#define PROFILER_H

/* Per-pass frame timings for the debug panel: CPU time (wall clock spent
   issuing the pass, quadtree walks included) and GPU time (GL timer
   queries, read back a few frames late so they never stall). Sections must
   not nest or overlap. */
typedef enum ProfSection {
	PROF_WAVES,      /* FFT ocean simulation */
	PROF_SHADOW,     /* sun shadow map */
	PROF_MOON,
	PROF_STANDINS,   /* planet stand-ins (planets.c): gas giants, far planets */
	PROF_PLANET,
	PROF_WATER,
	PROF_CLOUDS,     /* a rocky planet's weather cube (re-made now and then) and cloud shadow map */
	PROF_RINGS,
	PROF_ATMOSPHERE,
	PROF_NEBULA,      /* in-system nebula (nebula.c) */
	/* Inside a gas giant (gas_interior.c, cloud_volume.c), in frame order
	   (the volume's march, accumulate and composite on a rocky planet too): */
	PROF_CLOUD_WEATHER,   /* the cloud scene and its weather map (re-baked now and then) */
	PROF_CLOUD_SHADOW,    /* the clouds' shadow map */
	PROF_GAS_INTERIOR,    /* the haze */
	PROF_CLOUD_MARCH,     /* the volume's march, low resolution */
	PROF_CLOUD_ACCUM,     /* its accumulation over frames */
	PROF_CLOUD_COMPOSITE, /* upsampled over the scene */
	PROF_RESOLVE,    /* scene target MSAA resolve into the window */
	PROF_OVERLAY,    /* UI */
	PROF_COUNT
} ProfSection;

void Prof_Init(void);                 /* after GL is up */
void Prof_Begin(ProfSection s);
void Prof_End(ProfSection s);
void Prof_EndFrame(void);             /* once per frame, after the last section */

const char *Prof_Name(ProfSection s);
/* Smoothed milliseconds (exponential average over ~30 frames). */
float Prof_CpuMs(ProfSection s);
float Prof_GpuMs(ProfSection s);

/* Raw per-frame samples for the benchmark harness (off unless enabled):
   one GPU + CPU millisecond sample per section per frame, plus the frame's
   wall-clock time. GPU samples whose timer query came back on a disjoint
   interval are stored as -1. Samples arrive PROF_LAG frames late, so the
   harness must discard a warm-up window at the head of the run. */
#define PROF_WARMUP 60

void Prof_EnableRawRecording(void);
/* Write stats JSON: median/p95/p99 of the raw frame times plus per-pass
   medians/percentiles, skipping the warm-up frames. extraJson is a JSON
   fragment (without surrounding braces) appended as extra fields, or NULL.
   Returns 0 if the file could not be opened. */
int Prof_WriteJson(const char *path, const char *scene, const char *extraJson);

#endif
