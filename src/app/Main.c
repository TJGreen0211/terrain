#include "engine/core/gl.h"
#include "engine/gfx/LoadShader.h"
#include "engine/math/LinearAlg.h"
#include "engine/camera/arcballCamera.h"
#include "engine/platform/Window.h"
#include "engine/gfx/Sphere.h"
#include "engine/gfx/Geometry.h"
#include "engine/gfx/Obj.h"
#include "world/terrain/QuadCube.h"
#include "world/terrain/QuadTree.h"
#include "world/ocean/Water.h"
#include "engine/gfx/Common.h"
#include "engine/gfx/Textures.h"
#include "engine/gfx/Buffers.h"
#include "engine/gfx/CommonGL.h"
#include "engine/platform/Input.h"
#include "world/bodies/Sun.h"
#include "world/bodies/BodyDraw.h"
#include "engine/gfx/Frame.h"
#include "world/bodies/Planets.h"
#include "world/bodies/SolarSystem.h"
#include "world/sky/Skybox.h"
#include "world/sky/Nebula.h"
#include "world/atmosphere/Atmosphere.h"
#include "world/clouds/Clouds.h"
#include "app/LoadingScreen.h"
#include "engine/debug/GUI.h"
#include "app/DebugGUI.h"
#include "world/terrain/TerrainGen.h"
#include "world/terrain/TerrainDetail.h"
#include "world/terrain/TerrainDraw.h"
#include "world/ocean/WaterDraw.h"
#include "world/shadows/Shadows.h"
#include "world/rings/Rings.h"
#include "world/WorldConfig.h"
#include "world/bodies/Bodies.h"
#include "engine/debug/Profiler.h"
#include "app/Bench.h"
#include "engine/core/paths.h"
/* stb_image_write lives here; Bench.c uses it for its PNG captures. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stdio.h>
//#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <direct.h>
//#include <pthread.h>

/* World units are metres; every scale constant (camera near plane, shadow
   map size, body sizes and orbits, ...) is in WorldConfig.h. The main pass
   uses an infinite reversed-Z projection into a float depth buffer (see the
   scene target below), whose relative precision is the same at every
   distance, so the near plane can sit at surface scale while the sun and
   moon stay in view. */
#define CAMERA_NEAR ((float)CAMERA_NEAR_M)
struct sphere planet;
struct quadCube qc;

/* Camera-relative rendering (floating origin, ported from OpenGLSolar):
   world positions stay in double on the CPU, the camera is subtracted in
   double, and only the small difference is downcast to the float matrices
   the GPU sees. The view matrix is rotation only, so the camera sits at the
   origin of every shader space. g_cam is latched once per frame, before any
   draw, so every pass of a frame shares one origin. */
static dvec3 g_cam;

static vec3 toCamRel(dvec3 world) {
	return dvec3tovec3(dsubvec3(world, g_cam));
}

static mat4 camRelTranslate(dvec3 world) {
	return translatevec3(toCamRel(world));
}

/* Camera sliders (bottom-left, blue = movement speed on top, orange =
   scroll speed below), ported from OpenGLSolar. Log-mapped so the low end
   stays usable: t in [0,1] -> min*(max/min)^t. The ranges (WorldConfig.h)
   keep the camera defaults at the same relative spot OpenGLSolar's do. */
#define UI_SLIDER_MOVEMENT 0
#define UI_SLIDER_SCROLL   1
#define UI_SLIDER_COUNT    2

#define CAMERA_MOVEMENT_MIN ((float)CAMERA_MOVE_SPEED_MIN)
#define CAMERA_MOVEMENT_MAX ((float)CAMERA_MOVE_SPEED_MAX)
#define CAMERA_SCROLL_MIN ((float)CAMERA_SCROLL_STEP_MIN)
#define CAMERA_SCROLL_MAX ((float)CAMERA_SCROLL_STEP_MAX)

static float cameraSliderValue(float minV, float maxV, float t)
{
	return minV * powf(maxV / minV, t);
}

static float cameraSliderInitT(float minV, float maxV, float value)
{
	return logf(value / minV) / logf(maxV / minV);
}

void onCameraSliderChanged(int id, float t)
{
	if (id == UI_SLIDER_MOVEMENT)
		MovementSpeed = cameraSliderValue(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, t);
	else if (id == UI_SLIDER_SCROLL)
		CameraScrollSpeed = cameraSliderValue(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, t);
}

/* Accent colours double as labels: blue = movement, orange = scroll. */
static const float sliderAccentMovement[3] = { 0.45f, 0.65f, 1.0f };
static const float sliderAccentScroll[3] = { 1.0f, 0.55f, 0.30f };

void uiSetupCameraSliders()
{
	uiSliderSetChangedFn(onCameraSliderChanged);
	/* t0 puts each handle at the current camera default. */
	uiAddSlider(UI_SLIDER_MOVEMENT,
		cameraSliderInitT(CAMERA_MOVEMENT_MIN, CAMERA_MOVEMENT_MAX, MovementSpeed),
		sliderAccentMovement);
	uiAddSlider(UI_SLIDER_SCROLL,
		cameraSliderInitT(CAMERA_SCROLL_MIN, CAMERA_SCROLL_MAX, CameraScrollSpeed),
		sliderAccentScroll);
}

GLuint initQuadCube(int divisions) {
	GLuint vao;
	createCube(divisions, &qc);
	vec2 *texCoords = malloc(qc.vertexNumber*sizeof(vec2));
	vec3 *vna = malloc(qc.vertexNumber*sizeof(vec3));
	vec3 *tangent = malloc(qc.vertexNumber*sizeof(vec3));
	*tangent = *generateTangents(qc.vertexNumber, qc.points, tangent);

	for(int i = 0; i < qc.vertexNumber; i++) {
    	texCoords[i].x = (atan2(qc.points[i].y, qc.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(qc.points[i].z) / 3.1415926 + 0.5;
    }
    *vna = *generateSmoothNormals(vna, qc.points, qc.normals, qc.vertexNumber);
    //vao = initBuffers(qc.points, qc.size, vna, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);
    vao = initBufferTangents(qc.points, qc.size, vna, qc.nsize, tangent, qc.nsize, texCoords, sizeof(texCoords[0])*qc.vertexNumber);

    return vao;
}

GLuint initObjectBuffer(char *path, obj *object) {
	GLuint vao;
	*object = ObjLoadModel(path);
	vec3 *vna = malloc(object->vertexNumber*sizeof(vec3));
	vec2 *texCoords = malloc(object->vertexNumber*sizeof(vec2));
	*vna = *generateSmoothNormals(vna, object->points, object->normals, object->vertexNumber);

	for(int i = 0; i < object->vertexNumber; i++) {
    	texCoords[i].x = 1.0;
    	texCoords[i].y = 0.0;
    }
    vao = initBuffers(object->points, object->size, object->normals, object->nsize, texCoords, sizeof(texCoords[0])*object->vertexNumber);

	return vao;
}

GLuint initSphere() {
	planet = tetrahedron(5, &planet);
	GLuint vao;
    vec3 *vna = malloc(planet.vertexNumber*sizeof(vec3));
    vec2 *texCoords = malloc(planet.vertexNumber*sizeof(vec2));
    *vna = *generateSmoothNormals(vna, planet.points, planet.normals, planet.vertexNumber);

    for(int i = 0; i < planet.vertexNumber; i++) {
    	texCoords[i].x = (atan2(planet.points[i].y, planet.points[i].x) / 3.1415926 + 1.0) * 0.5;
    	texCoords[i].y = asin(planet.points[i].z) / 3.1415926 + 0.5;
    }

	vao = initBuffers(planet.points, planet.size, planet.normals, planet.nsize, texCoords, sizeof(texCoords[0])*planet.vertexNumber);
	return vao;
}

/* ---- Main-pass depth: reversed-Z into a float depth buffer ----
   The window's default depth buffer is 24-bit fixed point, which reversed-Z
   cannot help, so the scene renders into its own multisampled target (RGBA8
   + DEPTH32F) that is resolved into the window at the end of the frame.
   The depth is a multisample texture so the atmosphere dome can read it
   (Atmosphere_SetSceneDepth) to end its rays at the visible surface.
   Clip control is per pass: the main pass uses GL_ZERO_TO_ONE with
   GL_GREATER and a depth clear of 0; everything else (shadow map, bakes,
   GPU wave passes, UI) keeps the standard convention. */
#define SCENE_SAMPLES RENDER_MSAA_SAMPLES   /* WorldConfig.h */
static GLuint g_sceneFBO, g_sceneColorRB, g_sceneDepthTex;
static int g_sceneW, g_sceneH;

static void ensureSceneTarget(int w, int h) {
	if (g_sceneFBO && w == g_sceneW && h == g_sceneH)
		return;
	if (!g_sceneFBO) {
		glGenFramebuffers(1, &g_sceneFBO);
		glGenRenderbuffers(1, &g_sceneColorRB);
		glGenTextures(1, &g_sceneDepthTex);
	}
	g_sceneW = w;
	g_sceneH = h;
	glBindRenderbuffer(GL_RENDERBUFFER, g_sceneColorRB);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, SCENE_SAMPLES, GL_RGBA8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	/* Fixed sample locations: required to mix with the colour renderbuffer. */
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, g_sceneDepthTex);
	glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, SCENE_SAMPLES, GL_DEPTH_COMPONENT32F, w, h, GL_TRUE);
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFBO);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_sceneColorRB);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, g_sceneDepthTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[main] scene framebuffer incomplete\n");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	Atmosphere_SetSceneDepth(g_sceneDepthTex, SCENE_SAMPLES, CAMERA_NEAR);
	Nebula_SetSceneDepth(g_sceneDepthTex, SCENE_SAMPLES, CAMERA_NEAR);
}

/* Switches clip control, depth test and depth clear value between the main
   pass (reversed) and everything else (standard). Requires
   GL_ARB_clip_control (checked at startup). */
static void setReversedDepth(int on) {
	glClipControl(GL_LOWER_LEFT, on ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
	glDepthFunc(on ? GL_GREATER : GL_LESS);
	glClearDepth(on ? 0.0 : 1.0);
}

/* Main camera projection for every main-pass draw (infinite reversed-Z). */
static mat4 cameraProjection(void) {
	return perspectiveReversedInf((float)CAMERA_FOV_Y_DEG, getWindowWidth()/getWindowHeight(), CAMERA_NEAR);
}

/* Main camera projection * view (FrameContext.viewProj); the quadtree
   walks frustum-cull their nodes against it (with the infinite projection
   its far plane never culls). */
static mat4 getViewProjection(void) {
	return multiplymat4(cameraProjection(), getViewMatrix());
}

int main(int argc, char *argv[])
{
	struct obj rockObj[3];
	/* Simulation clock (seconds, double): drives every orbit and spin. */
	double simTime = 0.0;
	{
		CameraConfig cam = {
			CAMERA_DISTANCE_MAX_M, CAMERA_MOVE_SPEED_MPS, CAMERA_SCROLL_STEP_M,
			CAMERA_SPAWN_DISTANCE_M, EARTH_RADIUS_M,
			SURFACE_CAMERA_MIN_RADIUS_M, SURFACE_CAMERA_MAX_RADIUS_M, SURFACE_CAMERA_TILT_DEG
		};
		Camera_Init(&cam);
	}
	//printf("\tWorkdir: %s\n", getenv("PWD"));
	{
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		char *slash = strrchr(exePath, '\\');
		if(slash) *slash = '\0';
		_chdir(exePath);
	}
	Bench_ParseArgs(argc, argv);
	Nebula_ParseArgs(argc, argv);
	int benchW = 0, benchH = 0;
	int benchSize = Bench_FixedWindowSize(&benchW, &benchH);
	GLFWwindow *window = setupGLFW(benchSize ? "Bench" : "Shadows", benchW, benchH);
	if (window == NULL)
    {
        printf("Failed to create GLFW window.");
        glfwTerminate();
        return -1;
    }
	/* The debug panel sees input before the camera (Input.c). */
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
        return -1;
    }
	/* The main pass's reversed-Z depth (setReversedDepth) needs clip control. */
	if (!GLAD_GL_ARB_clip_control || !glClipControl)
	{
		fprintf(stderr, "GL_ARB_clip_control is required (reversed-Z depth) but not supported by this driver.\n");
		glfwTerminate();
		return -1;
	}
	if (Bench_Active())
		glfwSwapInterval(0);   /* no vsync: frames run as fast as they can */
	/* cube.exe --bake-sky <name>: bake the nebula sky's faces and exit. */
	if (Nebula_BakeRequested()) {
		int rc = Nebula_RunBake();
		glfwTerminate();
		return rc;
	}

	// Show the loading screen before the first blocking load, so the window
	// never sits blank/unresponsive while shaders compile, textures and models
	// decode, and the heightmap bakes. glfwPollEvents inside each frame keeps
	// the window responsive (Esc works) during loading.
	LoadingScreen_Init(window);
	LoadingScreen_Render(0.0f);

	GLuint sunNoiseTexture;

	struct waves ocean;
	initializeWaves(&ocean, 256, App_Time());
	LoadingScreen_Step();

	BodyDraw_Init();
	WaterDraw_Init();
	TerrainDraw_Init();
	attachGUIShaders();
	uiInit();
	uiSetupCameraSliders();
	DebugGUI_Init(window);
	LoadingScreen_Step(); // all shader programs
	/* The quadtree body LOD: six face grids + shared instance VBO, attribute
	   layout wired once (all three body programs declare vPosition at 0 and
	   the instance matrix at 4-7). */
	QuadTree_InitGL();
	Prof_Init();
	if (Bench_Active())
		Prof_EnableRawRecording();
	LoadingScreen_Step();

	initSun();
	Planets_Init();
	LoadingScreen_Step();

	initSkybox();
	LoadingScreen_Step();
	Nebula_Init();
	LoadingScreen_Step();
	/* A baked nebula sky (g_nebulaSkyName) also loads its settings. */
	GLuint skyboxTexture = loadSkyboxCube();
	LoadingScreen_Step();

	char assetPath[260];
	GLuint moonTex = loadTexture(Paths_Asset(assetPath, sizeof assetPath, "moon.jpg"), 0);
	LoadingScreen_Step();
	initSphere();   /* the `planet` mesh the atmosphere dome is built from */
	initAtmosphere(planet);
	LoadingScreen_Step();
	/* Ring rocks (Rings.c). Each mesh keeps its own obj: the vertex counts
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

	GLuint quadCubeVAO = initQuadCube(30);
	LoadingScreen_Step();

	Shadows_InitGL();
	GLuint sunFramebuffer = initFramebuffer(&sunNoiseTexture, 512, 512);
	LoadingScreen_Step();
	//GLuint quadVAO = initQuadVAO();

	/* Bake the planet height field: GPU shaping + CPU erosion + sea level
	   stats (TerrainGen.c), from g_terrainBake. The seed advances on every
	   H-key re-bake; the debug panel edits the rest and re-bakes on demand. */
	planetHeightTex = generatePlanetHeightmap(&g_terrainBake, &g_planetStyle, &g_planetGen, LoadingScreen_Step);
	TerrainDetail_Init();

	/* Procedural rings, seeded with the planet (re-rolled with it when the
	   seed changes). Every program that samples the ring shadow is a
	   receiver. */
	Rings_Init(g_terrainBake.seed, rockVAOs, rockVerts);
	Rings_AttachReceiver(TerrainDraw_SurfaceProgram());
	Rings_AttachReceiver(WaterDraw_Program());
	Rings_AttachReceiver(BodyDraw_Program());
	Rings_AttachReceiver(Atmosphere_Program());
	Clouds_Init();

	/* The procedural system (SolarSystem.c): Earth as baked above, every
	   other planet from the system seed (baked on approach). */
	SolarSystem_Init();

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	mat4 model;
	glViewport(0, 0, getWindowWidth(), getWindowHeight());

	float deltaTime = 0.0;
	float lastFrame = 0.0;
	g_waveParams[0] = 1.0f;   // shell radius = sea level (planet coast is at 1.0)
	g_waveParams[1] = 0.25f;  // ~0.0006 rms: well under the land relief
	g_waveParams[2] = 1.0f;
	g_waveParams[3] = 10.0f;  // wave scale: patches 10x true size (stylized)
	g_waveParams[4] = 1.0f;

	/* Never given storage: bound as the bodies' noiseTexture/detailNormal
	   (unit 2), it samples as zero. */
	GLuint dxWaveTex;
	glGenTextures(1, &dxWaveTex);

	// Force one final full-bar frame before handing over to the scene.
	LoadingScreen_Finish();
	glViewport(0, 0, getWindowWidth(), getWindowHeight());

	while(!glfwWindowShouldClose(window))
	{
		/* H: re-roll the active planet's terrain seed and re-bake it; the
		   debug panel's regenerate button re-bakes with its current
		   settings (1-3 s hitch either way). A new system seed re-generates
		   every planet but Earth. */
		int rebake = DebugGUI_ConsumeRegenerate();
		if (consumeRegenRequest()) {
			g_terrainBake.seed++;
			rebake = 1;
		}
		if (rebake)
			SolarSystem_RebakeActive(NULL);
		if (DebugGUI_ConsumeNewSystem()) {
			SolarSystem_Generate(g_systemSeed);
			/* The focused planet may have a new size: back off to see it. */
			resetCameraForFocus(CAMERA_FOCUS_DISTANCE_RADII * g_bodies[g_focusBody].radius);
		}

		/* Ocean waves: only Earth has water. */
		if (SolarSystem_ActiveBody() == BODY_EARTH) {
			Prof_Begin(PROF_WAVES);
			generateWaves(&ocean, App_Time());
			Prof_End(PROF_WAVES);
		}

		GLfloat currentFrame = (GLfloat)App_Time();
		deltaTime = currentFrame - lastFrame;
		lastFrame = currentFrame;
		doMovement(deltaTime);

		/* Advance the bodies (R pauses). The step is capped so a hitch (the
		   first frame after loading, a re-bake) does not jump hours ahead.
		   Bench runs pin simTime to the frame index: repeatable lighting. */
		if (Bench_Active())
			simTime = Bench_SimTime();
		else if (!getRotationPaused())
			simTime += fmin(deltaTime, 0.1) * g_simTimeScale;
		Bodies_Update(simTime);

		/* Focus body (Tab / debug panel): the camera rides with it. A
		   switch re-centres the orbit on the new body. */
		{
			static int lastFocus = BODY_EARTH;
			int step = consumeFocusStep();
			if (step != 0 && !Bench_Active())
				g_focusBody = ((g_focusBody + step) % BODY_COUNT + BODY_COUNT) % BODY_COUNT;
			const Body *f = &g_bodies[g_focusBody];
			dvec3 toSun = dsubvec3(g_bodies[BODY_SUN].position, f->position);
			double ds = dlengthvec3(toSun);
			dvec3 sunDir = ds > 0.0 ? dscalarvec3(toSun, 1.0 / ds) : (dvec3){ 1.0, 0.0, 0.0 };
			setCameraFocus(f->position, f->radius, sunDir);
			if (g_focusBody != lastFocus) {
				resetCameraForFocus(CAMERA_FOCUS_DISTANCE_RADII * f->radius);
				lastFocus = g_focusBody;
			}
		}
		Bench_UpdateCamera();

		/* This frame's floating origin (see camRelTranslate). */
		g_cam = getCameraPosition();

		/* The active planet (SolarSystem.c): the terrain planet drawn in full,
		   the largest on screen. One never seen up close bakes in the
		   background first (a few seconds as a stand-in; cached after).
		   Bench runs switch and bake at once: repeatable frames. */
		SolarSystem_Update(g_cam, g_focusBody, Bench_BlockingBakes());
		/* Cloud weather maps for the planets that have them (a build or
		   two per frame, as simulation time moves on). */
		Clouds_Update(g_simTime);
		int activeId = SolarSystem_ActiveBody();
		int isEarth = activeId == BODY_EARTH;
		const Body *planetB = &g_bodies[activeId];
		const Body *sun = &g_bodies[BODY_SUN];
		const Body *moon = &g_bodies[BODY_MOON];

		/* Full passes for the active planet and the Moon only while they are
		   more than a few pixels across; Planets_Draw stands in otherwise.
		   The Moon needs Earth's shadow map, so only with Earth active. */
		int planetFull = Planets_FullDetail(planetB, g_cam);
		int moonFull = isEarth && Planets_FullDetail(moon, g_cam);

		/* Planet radius (metres) and world centre. */
		float fScale = (float)planetB->radius;
		dvec3 translation = planetB->position;

		/* Atmosphere: shell starts at the planet surface; the rest of its
		   parameters live in g_atmosphere (Atmosphere.c, debug panel).
		   Shared by the dome (in-scatter) and the planet/water passes
		   (extinction). */
		float atmoInner = fScale;

		//int terrainMaxLOD = (int)(log(fScale)/log(2));

		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		glClearColor(0.0, 0.0, 0.0, 1.0);

		//Light calculations per frame: the sun body is the light
		dvec3 lightWorld = sun->position;
		/* Shading and shadows both use the camera-relative light. */
		vec3 lightPositionXYZ = toCamRel(lightWorld);
		vec4 lightPosition = {lightPositionXYZ.x, lightPositionXYZ.y, lightPositionXYZ.z, 1.0};

		/* What every pass of this frame shares (engine/gfx/Frame.h). */
		FrameContext frame;
		frame.cam = g_cam;
		frame.proj = cameraProjection();
		frame.view = getViewMatrix();
		frame.viewProj = getViewProjection();
		frame.nearPlane = CAMERA_NEAR;
		frame.viewportH = getWindowHeight();
		frame.lightPos = lightPosition;
		frame.time = App_Time();

		BodyDraw_SunMarker(quadCubeVAO, qc.vertexNumber, sunNoiseTexture, sun, &frame);

		/* Ring frame for this camera + light; ring-shadow receivers that are
		   not bound by their own draw calls bind here. */
		Rings_SetFrame(translation, g_cam, fScale, lightPosition);
		Rings_BindReceiver(BodyDraw_Program());
		Rings_BindReceiver(Atmosphere_Program());

		/* Planet model (scale * spin/tilt, camera-relative), composed in
		   double and downcast once; shared by the shadow casters and the
		   main pass. */
		QuadTree_Body planetBody = TerrainDraw_PlanetBody(planetB, g_cam);
		/* New frame: the walk cache must not outlive this frame's inputs. */
		QuadTree_BeginFrame();

		/* Shadow frustum: the sun is far enough away to be a directional
		   light, so an orthographic box along the sun direction, just wide
		   enough for the planet's bounding sphere (land relief + margin) so
		   all SHADOW_MAP_SIZE^2 texels land on the planet. Its eye sits a
		   finite distance up-sun (never at the sun itself: a 1 AU float
		   translation would quantise depth to kilometres). Depth spans the
		   moon's orbit on either side so it can both eclipse the planet and
		   be eclipsed by it. */
		{
			dvec3 toSun = dsubvec3(lightWorld, translation);
			dvec3 sunDir = dscalarvec3(toSun, 1.0 / dlengthvec3(toSun));
			double boundR = fScale * (1.0 + g_planetStyle.landRelief) * SHADOW_BOUND_MARGIN;
			/* Earth's Moon both casts and receives; other planets only self-shadow. */
			double reach = (isEarth ? dlengthvec3(dsubvec3(moon->position, translation)) + moon->radius * 1.5 : 0.0) + boundR;
			dvec3 eye = daddvec3(translation, dscalarvec3(sunDir, reach));
			Shadows_FitGlobal(eye, translation, g_cam, boundR, reach);
		}

		/* Shadow pass. The ring is not a caster: its shadow is semi-
		   transparent, so the receivers compute it analytically from the
		   ring profile instead (ringTransmittance). A slope-scaled polygon
		   offset handles the remaining grazing acne together with the
		   receiver offsets in sunVisibility. */
		/* Only while the planet or the Moon is drawn in full (the Moon receives Earth's shadow). */
		if (planetFull || moonFull) {
			Prof_Begin(PROF_SHADOW);
			Shadows_BeginGlobal();
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(1.5f, 2.0f);
				model = dmat4tomat4(Body_ModelMatrix(moon, g_cam));
				if (isEarth)
					Shadows_DrawMeshCaster(quadCubeVAO, qc.vertexNumber, model, Shadows_GlobalLightSpace());
				/* Coarse walk, no finer than half a texel: smaller detail cannot
				   show in this map. */
				TerrainDraw_ShadowCasters(&planetBody, Shadows_GlobalLightSpace(),
				                          0.5 * Shadows_GlobalTexel(), QT_SHADOW_TRIANGLE_PX, 1, &frame);

				/* Cascades: finer maps around the camera (Shadows.c), fitted
				   between the nearest and farthest terrain in view. Casters use
				   the main pass's LOD, so they are the very mesh that receives
				   (minus what each box culls), and keep everything up-sun of the
				   box (clamped onto its near plane). */
				{
					double nearest, farthest;
					TerrainDraw_ViewRange(planetB, g_cam, &nearest, &farthest);
					mat4 view = getViewMatrix();
					dvec3 forward = { -view.m[2][0], -view.m[2][1], -view.m[2][2] };
					dvec3 toSun = dsubvec3(lightWorld, translation);
					Shadows_Fit(g_cam, forward, dscalarvec3(toSun, 1.0 / dlengthvec3(toSun)), nearest, farthest);
				}
				for (int i = 0; i < g_cascades.count; i++) {
					const ShadowCascade *sc = &g_cascades.c[i];
					Shadows_BeginCascade(i);
					if (isEarth)
						Shadows_DrawMeshCaster(quadCubeVAO, qc.vertexNumber, model, sc->lightSpace);
					TerrainDraw_ShadowCasters(&planetBody, sc->lightSpace, 0.5 * sc->texel,
					                          QT_TARGET_TRIANGLE_PX, 0, &frame);
				}
				Shadows_EndCascades();
				glDisable(GL_POLYGON_OFFSET_FILL);
			Prof_End(PROF_SHADOW);
		}

		/* Main pass: the multisampled float-depth scene target, reversed-Z
		   (see setReversedDepth), resolved into the window before the UI. */
		/* Nebula presence grid on a settings change; Rebake / Save
		   requests from the debug panel (blocking). */
		Nebula_Update();

		int sceneW = (int)getWindowWidth(), sceneH = (int)getWindowHeight();
		ensureSceneTarget(sceneW, sceneH);
		glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFBO);
		glViewport(0, 0, sceneW, sceneH);
		setReversedDepth(1);

		glClearColor(0.1, 0.0, 0.2, 1.0);
		glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

		//Skybox: full-screen background, drawn first (far plane, no depth write)
		mat4 skyboxProj = cameraProjection();
		drawSkybox(Nebula_SkyTexture(skyboxTexture), skyboxProj, getViewRotation());

		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);
		if (moonFull) {
			Prof_Begin(PROF_MOON);
			BodyDraw_Moon(moonTex, moon, &frame);
			Prof_End(PROF_MOON);
		}
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		//Sun (billboard): opaque disc drawn with the scene; glow drawn last (below)
		mat4 sunProj = cameraProjection();
		mat4 sunView = getViewMatrix();
		float sunGlowRadius = (float)(sun->radius * SUN_GLOW_RADII);
		drawSunDisc(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

		//Distant bodies: every planet not drawn in full this frame (gas
		//giants always; terrain planets and the Moon while small or
		//inactive).
		Planets_Draw(sunProj, sunView, g_cam, planetFull ? activeId : -1, moonFull);

		//Quadtree planet: LOD sphere built from six instanced face grids,
		//centred camera-relative (the walk runs with the camera at the origin).
		glPolygonMode(GL_FRONT_AND_BACK, getWireframeState() ? GL_LINE : GL_FILL);

		//Planet (land + seafloor), then the water shell over it (Earth
		//only for now), alpha-blended over the seafloor.
		if (planetFull) {
			Prof_Begin(PROF_PLANET);
			TerrainDraw_Planet(&planetBody, &frame, atmoInner, activeId, dxWaveTex);
			Prof_End(PROF_PLANET);
		}
		if (planetFull && isEarth) {
			Prof_Begin(PROF_WATER);
			WaterDraw_Draw(&ocean, planetB, &frame, skyboxTexture, atmoInner, activeId);
			Prof_End(PROF_WATER);
		}
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		//Clouds: the layer as a slab over the planet (Clouds.c), blended over
		//the land and water, before the rings (which must draw over it).
		//From here on the scene's alpha holds the cloud cover the dome and
		//the sun's glow scale their light by: cleared to 0 here (every
		//frame: the glow always reads it), written only by the clouds (the
		//rings mask it off).
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		if (planetFull) {
			Prof_Begin(PROF_CLOUDS);
			Clouds_Draw(sunProj, sunView, g_cam, activeId);
			Prof_End(PROF_CLOUDS);
		}

		//Rings: opaque close-up rocks, then the analytic sheet and the dust
		//(premultiplied, depth-tested, no depth write). Before the
		//atmosphere dome, which takes the sheet's coverage out of its glow.
		if (planetFull) {
			Prof_Begin(PROF_RINGS);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   /* the cloud cover (above) */
			Rings_DrawRocks(sunProj, sunView);
			Rings_DrawSheet(sunProj, sunView);
			Rings_DrawDust(sunProj, sunView);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			Prof_End(PROF_RINGS);
		}

		//Nebula: the in-system wisps (Nebula.c), marched to the scene
		//depth and blended over everything drawn so far; the dome adds
		//its air in front afterwards. Not tied to planetFull: deep space
		//is where it matters most. The alpha channel keeps the cloud
		//cover (above), as for the rings.
		if (g_nebulaEnabled) {
			Prof_Begin(PROF_NEBULA);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			Nebula_Draw(sunProj, sunView, g_cam);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			Prof_End(PROF_NEBULA);
		}

		//Atmosphere dome: OpenGLSolar port (GPU Gems ch.16 scattering on the
		//planet shell; params above with the planet extinction). Rays end at
		//the scene depth, so this runs after everything opaque.
		if (planetFull && g_atmosphere.shellHeight > 0.0f) {
			Prof_Begin(PROF_ATMOSPHERE);
			drawAtmosphereDome(sunProj, sunView, camRelTranslate(translation),
			                   atmoInner, &g_atmosphere, lightPosition);
			Prof_End(PROF_ATMOSPHERE);
		}

		//Sun corona halo — must be drawn LAST (additive, does not write depth)
		drawSunGlow(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

		/* Resolve the scene into the window; the UI draws there directly,
		   with the standard depth convention restored. */
		Prof_Begin(PROF_OVERLAY);
		setReversedDepth(0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sceneFBO);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, sceneW, sceneH, 0, 0, sceneW, sceneH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		//Camera speed sliders: UI overlay, drawn over everything. Laid out
		//every frame so they track window resizes.
		{
			int ww, wh;
			glfwGetWindowSize(window, &ww, &wh);
			uiLayoutSliders(ww, wh, UI_SLIDER_COUNT);
		}
		Bench_MaybeCapture();
		uiDrawAll();

		//Debug panel (F1): drawn last, over the sliders.
		DebugGUI_Frame();
		Prof_End(PROF_OVERLAY);
		Prof_EndFrame();
		if (Bench_NoteFrame())
			glfwSetWindowShouldClose(window, GL_TRUE);

		glfwPollEvents();
		glfwSwapBuffers(window);
	}
	Bench_Finalize();
	DebugGUI_Shutdown();
	glfwTerminate();
	return 0;
}
