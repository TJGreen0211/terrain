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
	PROF_PLANET,
	PROF_WATER,
	PROF_RINGS,
	PROF_ATMOSPHERE,
	PROF_OVERLAY,    /* resolve blit + UI */
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

#endif
