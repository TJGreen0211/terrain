#include "app/app.h"
#include "app/bench.h"
#include "app/camera_sliders.h"
#include "engine/debug/debug_gui.h"
#include "engine/debug/nk.h"
#include "app/loading_screen.h"
#include "engine/camera/camera.h"
#include "engine/core/paths.h"
#include "engine/debug/ui.h"
#include "engine/debug/profiler.h"
#include "engine/gfx/buffer.h"
#include "engine/gfx/shader.h"
#include "engine/gfx/framebuffer.h"
#include "engine/gfx/mesh.h"
#include "engine/gfx/scene_target.h"
#include "engine/gfx/texture.h"
#include "engine/gfx/tone_map.h"
#include "engine/platform/frame_limiter.h"
#include "engine/platform/input.h"
#include "engine/platform/window.h"
#include "world/WorldConfig.h"
#include "world/atmosphere/atmosphere.h"
#include "world/bodies/body_draw.h"
#include "world/bodies/bodies.h"
#include "world/bodies/planets.h"
#include "world/bodies/solar_system.h"
#include "world/bodies/sun.h"
#include "world/clouds/clouds.h"
#include "world/clouds/cloud_volume.h"
#include "world/gasgiant/gas_giant.h"
#include "world/gasgiant/gas_interior.h"
#include "world/ocean/water_draw.h"
#include "world/rings/rings.h"
#include "world/shadows/shadows.h"
#include "world/sky/nebula.h"
#include "world/sky/skybox.h"
#include "world/terrain/quad_cube.h"
#include "world/terrain/quad_tree.h"
#include "world/terrain/terrain_detail.h"
#include "world/terrain/terrain_draw.h"
#include "world/terrain/terrain_gen.h"
#include "world/terrain/terrain_panel.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct sphere planet;
static struct quadCube qc;

static GLuint initQuadCube(int divisions) {
	GLuint vao;
	QuadCube_Create(divisions, &qc);
	vec2 *texCoords = malloc(qc.vertexNumber*sizeof(vec2));
	vec3 *vna = malloc(qc.vertexNumber*sizeof(vec3));
	vec3 *tangent = malloc(qc.vertexNumber*sizeof(vec3));
	*tangent = *Mesh_Tangents(qc.vertexNumber, qc.points, tangent);

	for(int i = 0; i < qc.vertexNumber; i++) {
    	texCoords[i].x = (atan2(qc.points[i].y, qc.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(qc.points[i].z) / 3.1415926 + 0.5;
    }
    *vna = *Mesh_SmoothNormals(vna, qc.points, qc.normals, qc.vertexNumber);
    //vao = Buffer_CreateMesh(qc.points, qc.size, vna, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);
    vao = Buffer_CreateMeshTangents(qc.points, qc.size, vna, qc.nsize, tangent, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);

    return vao;
}

static GLuint initObjectBuffer(char *path, obj *object) {
	GLuint vao;
	*object = Mesh_LoadObj(path);
	vec3 *vna = malloc(object->vertexNumber*sizeof(vec3));
	vec2 *texCoords = malloc(object->vertexNumber*sizeof(vec2));
	*vna = *Mesh_SmoothNormals(vna, object->points, object->normals, object->vertexNumber);

	for(int i = 0; i < object->vertexNumber; i++) {
    	texCoords[i].x = 1.0;
    	texCoords[i].y = 0.0;
    }
    vao = Buffer_CreateMesh(object->points, object->size, object->normals, object->nsize, texCoords, sizeof(texCoords[0])*object->vertexNumber);

	return vao;
}

static GLuint initSphere() {
	planet = Mesh_Tetrahedron(5, &planet);
	GLuint vao;
    vec3 *vna = malloc(planet.vertexNumber*sizeof(vec3));
    vec2 *texCoords = malloc(planet.vertexNumber*sizeof(vec2));
    *vna = *Mesh_SmoothNormals(vna, planet.points, planet.normals, planet.vertexNumber);

    for(int i = 0; i < planet.vertexNumber; i++) {
    	texCoords[i].x = (atan2(planet.points[i].y, planet.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(planet.points[i].z) / 3.1415926 + 0.5;
    }

	vao = Buffer_CreateMesh(planet.points, planet.size, planet.normals, planet.nsize, texCoords, sizeof(texCoords[0])*planet.vertexNumber);
	return vao;
}

/* The Camera tab of the debug panel: altitude above the focus body's
   sea-level sphere. */
static void cameraPanel(struct nk_context *ctx)
{
	const Body *focus = Bodies_Get(Bodies_Params()->focusBody);
	double alt = dlengthvec3(dsubvec3(Camera_Position(), focus->position)) - focus->radius;

	nk_layout_row_dynamic(ctx, DebugGUI_RowHeight(), 1);
	if (fabs(alt) < 1e4)
		nk_labelf(ctx, NK_TEXT_LEFT, "Altitude: %.1f m", alt);
	else
		nk_labelf(ctx, NK_TEXT_LEFT, "Altitude: %.1f km", alt / 1000.0);
}

/* Tone map tab: the engine's settings, then the gains of the passes still
   authored as display values (docs/hdr.md), so the look is tuned in one
   place. */
static void toneMapPanel(struct nk_context *ctx)
{
	ToneMap_DebugPanel(ctx);
	DebugGUI_Group(ctx, "Gains (display-authored passes)");
	DebugGUI_Slider(ctx, "Sky", Skybox_HdrGain(), 0.0f, 4.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Atmosphere", Atmosphere_HdrGain(), 0.0f, 4.0f, 0.01f, "%.2f");
	DebugGUI_Slider(ctx, "Sun", Sun_HdrGain(), 0.0f, 50.0f, 0.1f, "%.1f");
	DebugGUI_Slider(ctx, "Stand-ins", Planets_HdrGain(), 0.0f, 4.0f, 0.01f, "%.2f");
}

/* The debug panel's tabs, in order (below its built-in Performance tab). */
static void addDebugPanels(void)
{
	DebugGUI_AddPanel("Frame rate", FrameLimiter_DebugPanel, NULL);
	DebugGUI_AddPanel("Solar system", SolarSystem_DebugPanel, NULL);
	DebugGUI_AddPanel("Camera", cameraPanel, NULL);
	DebugGUI_AddPanel("Tone map", toneMapPanel, NULL);
	DebugGUI_AddPanel("Terrain", Terrain_DebugPanel, Terrain_DebugPanelStartup);
	DebugGUI_AddPanel("Shadows", Shadows_DebugPanel, NULL);
	DebugGUI_AddPanel("Atmosphere", Atmosphere_DebugPanel, NULL);
	DebugGUI_AddPanel("Clouds", Clouds_DebugPanel, NULL);
	DebugGUI_AddPanel("Gas giants", GasGiant_DebugPanel, NULL);
	DebugGUI_AddPanel("Nebula", Nebula_DebugPanel, NULL);
	DebugGUI_AddPanel("Water", Water_DebugPanel, Water_DebugPanelStartup);
}

int App_Create(App *app, int argc, char *argv[], int *exitCode)
{
	struct obj rockObj[3];
	memset(app, 0, sizeof *app);
	app->lastFocus = BODY_EARTH;
	*exitCode = 0;
	{
		CameraConfig cam = {
			CAMERA_DISTANCE_MAX_M, CAMERA_MOVE_SPEED_MPS, CAMERA_SCROLL_STEP_M,
			CAMERA_SPAWN_DISTANCE_M, EARTH_RADIUS_M,
			SURFACE_CAMERA_MIN_RADIUS_M, SURFACE_CAMERA_MAX_RADIUS_M, SURFACE_CAMERA_TILT_DEG
		};
		Camera_Init(&cam);
	}
	Bench_ParseArgs(argc, argv);
	Nebula_ParseArgs(argc, argv);
	int benchW = 0, benchH = 0;
	int benchSize = Bench_FixedWindowSize(&benchW, &benchH);
	GLFWwindow *window = Window_Create(benchSize ? "Bench" : "Shadows", benchW, benchH);
	if (window == NULL)
    {
        printf("Failed to create GLFW window.");
        glfwTerminate();
        *exitCode = -1;
        return 0;
    }
	app->window = window;
	/* The debug panel sees input before the camera (input.c). */
	{
		InputUiHooks hooks = {
			DebugGUI_KeyEvent, DebugGUI_CharEvent, DebugGUI_ScrollEvent,
			DebugGUI_MouseButtonEvent, DebugGUI_WantsMouse, DebugGUI_ToggleVisible
		};
		Input_SetUiHooks(&hooks);
	}
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        printf("Failed to initialize GLAD");
        *exitCode = -1;
        return 0;
    }
	/* The main pass's reversed-Z depth (render.c) needs clip control. */
	if (!GLAD_GL_ARB_clip_control || !glClipControl)
	{
		fprintf(stderr, "GL_ARB_clip_control is required (reversed-Z depth) but not supported by this driver.\n");
		glfwTerminate();
		*exitCode = -1;
		return 0;
	}
	/* Vsync off; the limiter paces normal runs, bench runs are uncapped
	   (main.c skips the wait). */
	FrameLimiter_Init();
	/* cube.exe --bake-sky <name>: bake the nebula sky's faces and exit. */
	if (Nebula_BakeRequested()) {
		*exitCode = Nebula_RunBake();
		glfwTerminate();
		return 0;
	}

	// Show the loading screen before the first blocking load, so the window
	// never sits blank/unresponsive while shaders compile, textures and models
	// decode, and the heightmap bakes. glfwPollEvents inside each frame keeps
	// the window responsive (Esc works) during loading.
	LoadingScreen_Init(window);
	LoadingScreen_Render(0.0f);

	/* Before anything draws: init-time bakes read the tone map block. */
	ToneMap_Init();
	/* --curve <n> (ToneMapCurve), --tonemap-debug <n> (ToneMapDebug),
	   e.g. for bench shots. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--curve") == 0 && i + 1 < argc)
			ToneMap_Params()->curve = atoi(argv[++i]) % TONEMAP_CURVE_COUNT;
		else if (strcmp(argv[i], "--tonemap-debug") == 0 && i + 1 < argc)
			ToneMap_Params()->debugView = atoi(argv[++i]) % TONEMAP_DEBUG_COUNT;
	}
	Water_Init(&app->ocean, 256, App_Time());
	LoadingScreen_Step();

	BodyDraw_Init();
	WaterDraw_Init();
	TerrainDraw_Init();
	UI_InitShaders();
	UI_Init();
	CameraSliders_Setup();
	DebugGUI_Init(window);
	addDebugPanels();
	LoadingScreen_Step(); // all shader programs
	/* The quadtree body LOD: six face grids + shared instance VBO, attribute
	   layout wired once (all three body programs declare vPosition at 0 and
	   the instance matrix at 4-7). */
	QuadTree_InitGL();
	Prof_Init();
	SceneTarget_Init(RENDER_MSAA_SAMPLES);
	if (Bench_Active())
		Prof_EnableRawRecording();
	LoadingScreen_Step();

	Sun_Init();
	GasGiant_Init();
	GasInterior_Init();
	Planets_Init();
	LoadingScreen_Step();

	Skybox_Init();
	LoadingScreen_Step();
	Nebula_Init();
	LoadingScreen_Step();
	/* A baked nebula sky (Nebula_SkyName()) also loads its settings. */
	app->skyboxTex = Skybox_LoadCube();
	LoadingScreen_Step();

	char assetPath[260];
	app->moonTex = Texture_Load(Paths_Asset(assetPath, sizeof assetPath, "moon.jpg"), 0);
	LoadingScreen_Step();
	initSphere();   /* the `planet` mesh the atmosphere dome is built from */
	Atmosphere_Init(planet);
	LoadingScreen_Step();
	/* Ring rocks (rings.c). Each mesh keeps its own obj: the vertex counts
	   differ. */
	GLuint rockVAOs[3];
	int rockVerts[3];
	rockVAOs[0] = initObjectBuffer(Paths_Asset(assetPath, sizeof assetPath, "rock.obj"), &rockObj[0]);
	LoadingScreen_Step();
	rockVAOs[1] = initObjectBuffer(Paths_Asset(assetPath, sizeof assetPath, "rock2.obj"), &rockObj[1]);
	LoadingScreen_Step();
	rockVAOs[2] = initObjectBuffer(Paths_Asset(assetPath, sizeof assetPath, "rock3.obj"), &rockObj[2]);
	LoadingScreen_Step();
	for (int i = 0; i < 3; i++)
		rockVerts[i] = rockObj[i].vertexNumber;

	app->quadCubeVAO = initQuadCube(30);
	app->quadCubeVerts = qc.vertexNumber;
	LoadingScreen_Step();

	Shadows_InitGL();
	app->sunFramebuffer = Framebuffer_Create(&app->sunNoiseTex, 512, 512);
	LoadingScreen_Step();

	/* Bake the planet height field: GPU shaping + CPU erosion + sea level
	   stats (terrain_gen.c), from TerrainGen_BakeSettings(). The seed advances on every
	   H-key re-bake; the debug panel edits the rest and re-bakes on demand. */
	TerrainDraw_SetHeightTex(TerrainGen_BakeHeightmap(TerrainGen_BakeSettings(), TerrainGen_Style(), TerrainGen_GenParams(), LoadingScreen_Step));
	TerrainDetail_Init();

	/* Procedural rings, seeded with the planet (re-rolled with it when the
	   seed changes). Every program that samples the ring shadow is a
	   receiver. */
	Rings_Init(TerrainGen_BakeSettings()->seed, rockVAOs, rockVerts);
	Rings_AttachReceiver(TerrainDraw_SurfaceProgram());
	Rings_AttachReceiver(WaterDraw_Program());
	Rings_AttachReceiver(BodyDraw_Program());
	Rings_AttachReceiver(Atmosphere_Program());
	Clouds_Init();
	CloudVolume_Init();

	/* The procedural system (solar_system.c): Earth as baked above, every
	   other planet from the system seed (baked on approach). */
	SolarSystem_Init();

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	glViewport(0, 0, Window_Width(), Window_Height());

	Water_Params()[0] = 1.0f;   // shell radius = sea level (planet coast is at 1.0)
	Water_Params()[1] = 0.25f;  // ~0.0006 rms: well under the land relief
	Water_Params()[2] = 1.0f;
	Water_Params()[3] = 10.0f;  // wave scale: patches 10x true size (stylized)
	Water_Params()[4] = 1.0f;

	glGenTextures(1, &app->dxWaveTex);

	// Force one final full-bar frame before handing over to the scene.
	LoadingScreen_Finish();
	glViewport(0, 0, Window_Width(), Window_Height());
	return 1;
}

void App_Update(App *app)
{
	/* H: re-roll the active planet's terrain seed and re-bake it; the
	   debug panel's regenerate button re-bakes with its current
	   settings (1-3 s hitch either way). A new system seed re-generates
	   every planet but Earth. */
	int rebake = Terrain_ConsumeRegenerate();
	if (Input_ConsumeRegenRequest()) {
		TerrainGen_BakeSettings()->seed++;
		rebake = 1;
	}
	if (rebake)
		SolarSystem_RebakeActive(NULL);
	if (SolarSystem_ConsumeNewSystem()) {
		SolarSystem_Generate(SolarSystem_Params()->seed);
		/* The focused planet may have a new size: back off to see it. */
		Camera_ResetForFocus(CAMERA_FOCUS_DISTANCE_RADII * Bodies_Get(Bodies_Params()->focusBody)->radius);
	}

	/* Ocean waves: only for an active planet with seas. */
	if (SolarSystem_HasWater(SolarSystem_ActiveBody())) {
		Prof_Begin(PROF_WAVES);
		Water_Generate(&app->ocean, App_Time());
		Prof_End(PROF_WAVES);
	}

	GLfloat currentFrame = (GLfloat)App_Time();
	float deltaTime = currentFrame - app->lastFrame;
	app->lastFrame = currentFrame;
	Input_ApplyMovement(deltaTime);

	/* Advance the bodies (R pauses). The step is capped so a hitch (the
	   first frame after loading, a re-bake) does not jump hours ahead.
	   Bench runs pin simTime to the frame index: repeatable lighting. */
	if (Bench_Active())
		app->simTime = Bench_SimTime();
	else if (!Input_RotationPaused())
		app->simTime += fmin(deltaTime, 0.1) * Bodies_Params()->timeScale;
	Bodies_Update(app->simTime);

	/* Focus body (Tab / debug panel): the camera rides with it. A
	   switch re-centres the orbit on the new body. */
	{
		int step = Input_ConsumeFocusStep();
		if (step != 0 && !Bench_Active())
			Bodies_Params()->focusBody = ((Bodies_Params()->focusBody + step) % BODY_COUNT + BODY_COUNT) % BODY_COUNT;
		const Body *f = Bodies_Get(Bodies_Params()->focusBody);
		dvec3 toSun = dsubvec3(Bodies_Get(BODY_SUN)->position, f->position);
		double ds = dlengthvec3(toSun);
		dvec3 sunDir = ds > 0.0 ? dscalarvec3(toSun, 1.0 / ds) : (dvec3){ 1.0, 0.0, 0.0 };
		Camera_SetFocus(f->position, f->radius, sunDir);
		if (Bodies_Params()->focusBody != app->lastFocus) {
			Camera_ResetForFocus(CAMERA_FOCUS_DISTANCE_RADII * f->radius);
			app->lastFocus = Bodies_Params()->focusBody;
		}
	}
	Bench_UpdateCamera();

	/* Near a gas giant: slower flight among its clouds, and never below
	   the floor under them (gas_interior.c). */
	{
		double alt;
		int giant = GasInterior_Body(Camera_Position(), &alt);
		if (giant >= 0)
			Camera_KeepOutside(Bodies_Get(giant)->position, GasInterior_FloorRadius(giant));
		Camera_SetSpeedScale(giant >= 0 ? GasInterior_SpeedScale(alt) : 1.0);
	}

	/* This frame's floating origin (render.c). */
	app->cam = Camera_Position();

	/* The active planet (solar_system.c): the terrain planet drawn in full,
	   the largest on screen. One never seen up close bakes in the
	   background first (a few seconds as a stand-in; cached after).
	   Bench runs switch and bake at once: repeatable frames. */
	SolarSystem_Update(app->cam, Bodies_Params()->focusBody, Bench_BlockingBakes());
	/* Cloud weather maps for the planets that have them (a build or
	   two per frame, as simulation time moves on). */
	Clouds_Update(Bodies_SimTime());
}

void App_Destroy(App *app)
{
	(void)app;
	Bench_Finalize();
	DebugGUI_Shutdown();
	FrameLimiter_Shutdown();
	glfwTerminate();
}
