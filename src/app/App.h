#ifndef APP_H
#define APP_H
#include "engine/core/gl.h"

#include "engine/math/LinearAlg.h"
#include "world/ocean/Water.h"

/* The composition root's state (App.c): the window, the meshes and
   textures only the app's passes use, and the simulation clock. Everything
   else lives in the module that owns it. */
typedef struct App {
	GLFWwindow *window;
	struct waves ocean;
	GLuint quadCubeVAO;      /* sun marker + moon shadow caster */
	int quadCubeVerts;
	GLuint sunNoiseTex;      /* sun marker texture (its framebuffer's colour) */
	GLuint sunFramebuffer;
	GLuint skyboxTex;
	GLuint moonTex;
	/* Never given storage: bound as the bodies' noiseTexture/detailNormal
	   (unit 2), it samples as zero. */
	GLuint dxWaveTex;
	/* Simulation clock (seconds, double): drives every orbit and spin. */
	double simTime;
	float lastFrame;         /* App_Time() of the previous update */
	int lastFocus;           /* focus body of the previous update */
	/* This frame's floating origin (Render.c), latched by App_Update before
	   any draw, so every pass of a frame shares one origin. */
	dvec3 cam;
} App;

/* Window, GL, every module and the loading screen. Returns 1 to run the
   loop; 0 when it should not run (a failure, or a one-shot mode such as
   --bake-sky that has already finished), with the process exit code in
   *exitCode. */
int App_Create(App *app, int argc, char *argv[], int *exitCode);
/* Input, re-bakes, waves, clocks, bodies, camera and the active planet:
   everything a frame changes before it draws. */
void App_Update(App *app);
void App_Destroy(App *app);

#endif
