#ifndef BENCH_H
#define BENCH_H

/* Headless benchmark mode: cube.exe --bench <scene> runs a scripted camera
   path on a fixed virtual clock (1/60 s per frame), records raw per-frame
   timings, optionally captures PNGs at fixed frames, writes a JSON report
   and exits. Every run of a given scene renders the same frames: time
   uniforms, the sim clock and the camera all derive from the frame index,
   so the harness is repeatable and comparable across builds.

   Scene files (bench/scenes/<name>.bench), one directive per line:
     view   orbit | surface      which camera rig to drive
     pivot  earth | subsolar     orbit pivot: planet centre, or a point on
                                 the sub-solar surface (always daytime)
     poff   <metres>             radial offset of the subsolar pivot (can
                                 be negative: inside the planet gives a
                                 grazing near-horizontal view)
     phase  <deg>                sub-solar phase offset (default 0)
     localup                     subsolar pivot: the screen's up is the
                                 pivot's vertical (level horizon), not
                                 world +Y
     frames <n>                  run length (default 480, --frames wins)
     simtime <seconds>           simulation time at frame 0
     bakeasync                   planets bake in the background as in
                                 normal use (frames stop being repeatable;
                                 default: they bake at once)
     key <frame> <dist> <yawDeg> <pitchDeg>   orbit pose (>= 2 keys)
     skey <frame> <altM>                      surface camera radius

   App_Time() is the scene clock: virtual frames/60 while benching, the wall
   clock otherwise. Every animated shader input must read it, never
   glfwGetTime() directly, or bench runs stop being repeatable. */

double App_Time(void);

void Bench_ParseArgs(int argc, char *argv[]);   /* before window creation */
int Bench_Active(void);
int Bench_BlockingBakes(void);                   /* bench run without bakeasync */
long Bench_Frame(void);                         /* 0-based, current frame */

/* window.c consults this before creating the window: bench runs in a
   hidden fixed-size window so resolution and OS scaling cannot vary. */
int Bench_FixedWindowSize(int *w, int *h);

/* Drive the scripted camera + sim clock for the current frame. Call after
   Bodies_Update() (it reads the earth's position). No-op when not benching. */
void Bench_UpdateCamera(void);
double Bench_SimTimeStart(void);
/* The sim clock for the current frame: start + frame time x BENCH_TIME_SCALE
   x the scene's 'timescale' (0 freezes the bodies). */
double Bench_SimTime(void);

/* Capture any PNG requested for the current frame. Call after the scene
   resolve blit, before the UI draws. No-op when not benching. */
void Bench_MaybeCapture(void);

/* Record this frame's quadtree stats; returns 1 when the run is finished
   (the loop should then close the window). 0 when not benching. */
int Bench_NoteFrame(void);

/* Write the JSON report (call while the GL context is still current). */
void Bench_Finalize(void);

#endif
