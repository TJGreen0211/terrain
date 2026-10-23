/* Headless benchmark harness: see bench.h. Everything animated in a bench
   run derives from the frame index, so two runs of a build render identical
   frames and their timings are comparable. */

#include "app/bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "engine/core/gl.h"

#include "engine/platform/window.h"
#include "engine/math/linalg.h"
#include "engine/camera/camera.h"
#include "world/bodies/bodies.h"
#include "world/bodies/solar_system.h"
#include "world/WorldConfig.h"
#include "world/terrain/quad_tree.h"
#include "world/terrain/terrain_draw.h"
#include "engine/debug/profiler.h"
#include "engine/core/paths.h"
#include <stb_image_write.h>

void testSetSurfaceRadius(float r);

#define MAX_KEYS 128
#define MAX_SHOTS 16
#define BENCH_DT (1.0 / 60.0)

typedef struct { double frame, dist, yaw, pitch; } PoseKey;
typedef struct { double frame, alt; } AltKey;

static int active;
static int bakeAsync;             /* bakeasync: planets bake in the background */
static long frame;
static long totalFrames;          /* 0 until resolved (scene then CLI wins) */
static long cliFrames;
static char sceneName[128] = "bench";
static char outPath[512];
static char shotDir[512];         /* --shots-dir, else <bench root>/cand */

static int viewSurface;           /* 0 orbit, 1 surface preset */
static int pivotSubsolar;
static int pivotBody = BODY_EARTH;  /* pivot <body name>: orbit it (and focus it) */
/* pivotat <frame> <body name>: switch the pivot body from that frame on. */
static long switchFrame[MAX_KEYS]; static int switchBody[MAX_KEYS]; static int nSwitch;
static int startBody = BODY_EARTH;
static double pivotOff, phaseDeg, pivotLatDeg, simStart;
static int pivotGround;           /* poff ground: pivotOff is above the terrain */
static int localUp;               /* localup: subsolar camera's up = the pivot's vertical */
static double timeScale = 1.0;
static PoseKey keys[MAX_KEYS]; static int nKeys;
static AltKey akeys[MAX_KEYS];  static int nAKeys;
static long shots[MAX_SHOTS];   static int nShots;
static int winW = 1280, winH = 720;

static long leavesMax; static int levelMax;
/* Quadtree walk totals over the run (CPU ms, nodes visited / built). */
static double walkMsSum, nodesSum, builtSum;

/* ---------------- clock ---------------- */

double App_Time(void)
{
	return active ? (double)frame * BENCH_DT : glfwGetTime();
}

/* ---------------- scene file ---------------- */

static double lerp2(double f0, double v0, double f1, double v1, double f)
{
	if (f1 <= f0) return v1;
	return v0 + (v1 - v0) * ((f - f0) / (f1 - f0));
}

static void interpPose(double f, double *dist, double *yaw, double *pitch)
{
	if (nKeys == 0) { *dist = 0; *yaw = 0; *pitch = 0; return; }
	if (f <= keys[0].frame) {
		*dist = keys[0].dist; *yaw = keys[0].yaw; *pitch = keys[0].pitch; return;
	}
	if (f >= keys[nKeys - 1].frame) {
		*dist = keys[nKeys - 1].dist; *yaw = keys[nKeys - 1].yaw; *pitch = keys[nKeys - 1].pitch; return;
	}
	int k = 0;
	while (k + 1 < nKeys && keys[k + 1].frame < f) k++;
	*dist = lerp2(keys[k].frame, keys[k].dist, keys[k + 1].frame, keys[k + 1].dist, f);
	*yaw = lerp2(keys[k].frame, keys[k].yaw, keys[k + 1].frame, keys[k + 1].yaw, f);
	*pitch = lerp2(keys[k].frame, keys[k].pitch, keys[k + 1].frame, keys[k + 1].pitch, f);
}

static double interpAlt(double f)
{
	if (nAKeys == 0) return (double)SURFACE_CAMERA_MIN_RADIUS_M;
	if (f <= akeys[0].frame) return akeys[0].alt;
	if (f >= akeys[nAKeys - 1].frame) return akeys[nAKeys - 1].alt;
	int k = 0;
	while (k + 1 < nAKeys && akeys[k + 1].frame < f) k++;
	return lerp2(akeys[k].frame, akeys[k].alt, akeys[k + 1].frame, akeys[k + 1].alt, f);
}

static void stripComment(char *line)
{
	for (char *p = line; *p; p++)
		if (p[0] == '#' || (p[0] == '/' && p[1] == '/')) { *p = '\0'; return; }
}

static int loadScene(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	nKeys = nAKeys = 0;
	char line[512];
	long sceneFrames = 0;
	while (fgets(line, sizeof line, f)) {
		stripComment(line);
		char tok[64];
		if (sscanf(line, "%63s", tok) != 1) continue;
		if (strcmp(tok, "view") == 0) {
			char v[64] = "orbit";
			sscanf(line, "%*s %63s", v);
			viewSurface = strcmp(v, "surface") == 0;
		} else if (strcmp(tok, "pivot") == 0) {
			char v[64] = "earth", w[64] = "";
			sscanf(line, "%*s %63s %63s", v, w);
			pivotSubsolar = strcmp(v, "subsolar") == 0;
			/* "pivot subsolar <body>": that body's sub-solar point. */
			if (pivotSubsolar && w[0])
				strcpy(v, w);
			/* Any body by name (case-insensitive): orbit its centre and
			   make it the focus. "earth" / "subsolar" keep Earth. */
			pivotBody = BODY_EARTH;
			for (int i = 0; i < BODY_COUNT; i++)
				if (_stricmp(v, Bodies_Get(i)->name) == 0)
					pivotBody = i;
		} else if (strcmp(tok, "poff") == 0) {
			char v[64] = "";
			sscanf(line, "%*s %63s", v);
			pivotGround = strcmp(v, "ground") == 0;
			sscanf(line, pivotGround ? "%*s %*s %lf" : "%*s %lf", &pivotOff);
		} else if (strcmp(tok, "phase") == 0) {
			sscanf(line, "%*s %lf", &phaseDeg);
		} else if (strcmp(tok, "pivotlat") == 0) {
			/* "pivotlat <deg>": tilt the sub-solar pivot toward world +Y
			   (north) by deg, e.g. to put a coast under the camera. */
			sscanf(line, "%*s %lf", &pivotLatDeg);
		} else if (strcmp(tok, "frames") == 0) {
			sscanf(line, "%*s %ld", &sceneFrames);
		} else if (strcmp(tok, "pivotat") == 0 && nSwitch < MAX_KEYS) {
			char v[64] = "";
			long f = 0;
			if (sscanf(line, "%*s %ld %63s", &f, v) == 2)
				for (int i = 0; i < BODY_COUNT; i++)
					if (_stricmp(v, Bodies_Get(i)->name) == 0) {
						switchFrame[nSwitch] = f;
						switchBody[nSwitch++] = i;
					}
		} else if (strcmp(tok, "localup") == 0) {
			localUp = 1;
		} else if (strcmp(tok, "bakeasync") == 0) {
			bakeAsync = 1;
		} else if (strcmp(tok, "systemseed") == 0) {
			sscanf(line, "%*s %u", &SolarSystem_Params()->seed);
		} else if (strcmp(tok, "timescale") == 0) {
			sscanf(line, "%*s %lf", &timeScale);
		} else if (strcmp(tok, "simtime") == 0) {
			sscanf(line, "%*s %lf", &simStart);
		} else if (strcmp(tok, "key") == 0 && nKeys < MAX_KEYS) {
			if (sscanf(line, "%*s %lf %lf %lf %lf",
			           &keys[nKeys].frame, &keys[nKeys].dist,
			           &keys[nKeys].yaw, &keys[nKeys].pitch) == 4) {
				/* Keep yaw on the shortest path between keys. */
				if (nKeys > 0) {
					while (keys[nKeys].yaw - keys[nKeys - 1].yaw > 180.0)  keys[nKeys].yaw -= 360.0;
					while (keys[nKeys].yaw - keys[nKeys - 1].yaw < -180.0) keys[nKeys].yaw += 360.0;
				}
				nKeys++;
			}
		} else if (strcmp(tok, "skey") == 0 && nAKeys < MAX_KEYS) {
			if (sscanf(line, "%*s %lf %lf", &akeys[nAKeys].frame, &akeys[nAKeys].alt) == 2)
				nAKeys++;
		}
	}
	fclose(f);
	if (cliFrames > 0) totalFrames = cliFrames;
	else if (sceneFrames > 0) totalFrames = sceneFrames;
	else totalFrames = 480;
	if (viewSurface && nAKeys == 0) {
		akeys[0].frame = 0; akeys[0].alt = (double)SURFACE_CAMERA_MIN_RADIUS_M;
		akeys[1].frame = (double)totalFrames; akeys[1].alt = (double)SURFACE_CAMERA_MIN_RADIUS_M;
		nAKeys = 2;
	}
	if (!viewSurface && nKeys < 2) {
		fprintf(stderr, "[bench] scene %s needs at least two 'key' lines\n", path);
		return 0;
	}
	Bodies_Params()->focusBody = pivotBody;
	startBody = pivotBody;
	if (phaseDeg != 0.0) {
		char buf[64];
		sprintf(buf, "%g", phaseDeg);
		_putenv_s("QT_TEST_PHASE", buf);
	}
	return 1;
}

static void sceneNameFromPath(const char *path)
{
	const char *base = strrchr(path, '/');
	const char *bs = strrchr(path, '\\');
	if (bs && (!base || bs > base)) base = bs;
	if (base) base++; else base = path;
	strncpy(sceneName, base, sizeof sceneName - 1);
	sceneName[sizeof sceneName - 1] = '\0';
	char *dot = strrchr(sceneName, '.');
	if (dot) *dot = '\0';
}

/* ---------------- args ---------------- */

void Bench_ParseArgs(int argc, char *argv[])
{
	const char *scene = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) scene = argv[++i];
		else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) cliFrames = atol(argv[++i]);
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) strncpy(outPath, argv[++i], sizeof outPath - 1);
		else if (strcmp(argv[i], "--shots-dir") == 0 && i + 1 < argc) strncpy(shotDir, argv[++i], sizeof shotDir - 1);
		else if (strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
			char *list = argv[++i], *ctx = NULL;
			for (char *t = strtok_s(list, ",", &ctx); t && nShots < MAX_SHOTS; t = strtok_s(NULL, ",", &ctx))
				shots[nShots++] = atol(t);
		}
		else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) winW = (int)atol(argv[++i]);
		else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) winH = (int)atol(argv[++i]);
	}
	if (!scene) return;
	active = 1;

	/* Resolve the scene: exact path, then bench/scenes/<name>[.bench]. */
	char path[512];
	if (loadScene(scene)) {
		strncpy(path, scene, sizeof path - 1);
	} else {
		Paths_Bench(path, sizeof path, "scenes/%s.bench", scene);
		if (!loadScene(path)) {
			Paths_Bench(path, sizeof path, "scenes/%s", scene);
			if (!loadScene(path)) {
				fprintf(stderr, "[bench] cannot find scene '%s' (tried as a path and in %sscenes)\n", scene, Paths_BenchRoot());
				exit(2);
			}
		}
	}
	sceneNameFromPath(path);
	if (outPath[0] == '\0') {
		Paths_Bench(outPath, sizeof outPath, "out/%s.json", sceneName);
	}
	fprintf(stderr, "[bench] scene %s: %ld frames, %s view, %s pivot\n",
	        sceneName, totalFrames, viewSurface ? "surface" : "orbit",
	        pivotSubsolar ? "subsolar" : Bodies_Get(pivotBody)->name);
}

/* ---------------- accessors ---------------- */

int Bench_Active(void) { return active; }
int Bench_BlockingBakes(void) { return active && !bakeAsync; }
long Bench_Frame(void) { return frame; }
double Bench_SimTimeStart(void) { return simStart; }
double Bench_SimTime(void) { return simStart + (double)frame * BENCH_DT * BENCH_TIME_SCALE * timeScale; }

int Bench_FixedWindowSize(int *w, int *h)
{
	if (!active) return 0;
	*w = winW; *h = winH;
	return 1;
}

/* ---------------- per-frame ---------------- */

void Bench_UpdateCamera(void)
{
	if (!active) return;
	pivotBody = startBody;
	for (int i = 0; i < nSwitch; i++)
		if (frame >= switchFrame[i])
			pivotBody = switchBody[i];
	Bodies_Params()->focusBody = pivotBody;
	const Body *e = Bodies_Get(pivotBody);
	if (viewSurface) {
		if (!Camera_IsSurface()) Camera_ToggleSurface();
		testSetSurfaceRadius((float)interpAlt((double)frame));
		return;
	}
	double dist, yaw, pitch;
	interpPose((double)frame, &dist, &yaw, &pitch);
	dvec3 pivot = Bodies_Get(pivotBody)->position;
	if (pivotSubsolar) {
		/* True sub-solar point: the direction of the sun body itself, so
		   the pivot is lit at any simTime (the phase key then slides the
		   pivot toward the terminator). The scene's yaw/pitch are the
		   camera's azimuth around the pivot (0 = east, 90 = straight
		   above, in the pivot's local frame) and its elevation out of
		   that frame toward +Y: at an equatorial pivot the local "up" is
		   horizontal, so it cannot be expressed with world pitch. */
		dvec3 toSun = dsubvec3(Bodies_Get(BODY_SUN)->position, e->position);
		double psi = atan2(toSun.z, toSun.x) + phaseDeg * (M_PI / 180.0);
		double lat = pivotLatDeg * (M_PI / 180.0);
		dvec3 dir = { cos(lat) * cos(psi), sin(lat), cos(lat) * sin(psi) };
		dvec3 east = { -sin(psi), 0.0, cos(psi) };
		double phi = yaw * (M_PI / 180.0), el = pitch * (M_PI / 180.0);
		dvec3 u;
		u.x = cos(el) * (cos(phi) * east.x + sin(phi) * dir.x);
		u.y = sin(el);
		u.z = cos(el) * (cos(phi) * east.z + sin(phi) * dir.z);
		/* poff ground: from the highest terrain within ~1 km instead of
		   sea level (use with timescale 0, or the ground slides under). */
		double base = pivotGround ? Earth_GroundRadius(dir, 1000.0 / e->radius) : e->radius;
		pivot = daddvec3(e->position, dscalarvec3(dir, base + pivotOff));
		Camera_SetPivot(pivot);
		if (localUp)
			Camera_SetOrbitEyeDirUp(u.x, u.y, u.z, dir.x, dir.y, dir.z, (float)dist);
		else
			Camera_SetOrbitEyeDir(u.x, u.y, u.z, (float)dist);
		return;
	}
	Camera_SetPivot(pivot);
	Camera_SetOrbitPose(yaw, pitch, (float)dist);
}

void Bench_MaybeCapture(void)
{
	if (!active) return;
	int hit = 0;
	for (int i = 0; i < nShots; i++)
		if (shots[i] == frame) { hit = 1; break; }
	if (!hit) return;

	int w = Window_Width(), h = Window_Height();
	unsigned char *px = malloc((size_t)w * h * 4);
	if (!px) return;
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	stbi_flip_vertically_on_write(1);
	char path[640];
	if (!shotDir[0])
		Paths_Bench(shotDir, sizeof shotDir, "cand");
	sprintf(path, "%s/%s_f%ld.png", shotDir, sceneName, frame);
	if (stbi_write_png(path, w, h, 4, px, w * 4))
		fprintf(stderr, "[bench] shot %s\n", path);
	free(px);
}

int Bench_NoteFrame(void)
{
	if (!active) return 0;
	if (QuadTree_FrameStats()->leaves > leavesMax) leavesMax = QuadTree_FrameStats()->leaves;
	if (QuadTree_FrameStats()->maxLevel > levelMax) levelMax = QuadTree_FrameStats()->maxLevel;
	walkMsSum += QuadTree_FrameStats()->walkMs;
	nodesSum += QuadTree_FrameStats()->nodes;
	builtSum += QuadTree_FrameStats()->built;
	frame++;
	return frame >= totalFrames;
}

void Bench_Finalize(void)
{
	if (!active) return;
	double simEnd = Bench_SimTime();
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	char extra[1024];
	snprintf(extra, sizeof extra,
	         "\"width\":%d,\"height\":%d,\"renderer\":\"%s\",\"glVersion\":\"%s\","
	         "\"framesRun\":%ld,\"simTimeStart\":%.1f,\"simTimeEnd\":%.1f,"
	         "\"leavesMax\":%ld,\"maxLevel\":%d,"
	         "\"walkMsAvg\":%.3f,\"nodesAvg\":%.0f,\"builtAvg\":%.0f",
	         winW, winH, renderer ? renderer : "?", version ? version : "?",
	         frame, simStart, simEnd, leavesMax, levelMax,
	         walkMsSum / fmax(frame, 1), nodesSum / fmax(frame, 1), builtSum / fmax(frame, 1));
	if (Prof_WriteJson(outPath, sceneName, extra))
		fprintf(stderr, "[bench] wrote %s\n", outPath);
	else
		fprintf(stderr, "[bench] FAILED to write %s (missing directory?)\n", outPath);
}
