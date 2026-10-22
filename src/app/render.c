#include "app/render.h"
#include "app/bench.h"
#include "app/camera_sliders.h"
#include "engine/debug/debug_gui.h"
#include "engine/camera/camera.h"
#include "engine/debug/ui.h"
#include "engine/debug/profiler.h"
#include "engine/gfx/frame.h"
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
#include "world/ocean/water_draw.h"
#include "world/rings/rings.h"
#include "world/shadows/shadows.h"
#include "world/sky/nebula.h"
#include "world/sky/skybox.h"
#include "world/terrain/quad_tree.h"
#include "world/terrain/terrain_draw.h"
#include "world/terrain/terrain_gen.h"
#include <stdio.h>

/* World units are metres; every scale constant (camera near plane, shadow
   map size, body sizes and orbits, ...) is in WorldConfig.h. The main pass
   uses an infinite reversed-Z projection into a float depth buffer (see the
   scene target below), whose relative precision is the same at every
   distance, so the near plane can sit at surface scale while the sun and
   moon stay in view. */
#define CAMERA_NEAR ((float)CAMERA_NEAR_M)

/* Camera-relative rendering (floating origin, ported from OpenGLSolar):
   world positions stay in double on the CPU, the camera is subtracted in
   double, and only the small difference is downcast to the float matrices
   the GPU sees. The view matrix is rotation only, so the camera sits at the
   origin of every shader space. App_Update latches app->cam once per frame,
   before any draw, so every pass of a frame shares one origin. */
static vec3 toCamRel(dvec3 world, dvec3 cam) {
	return dvec3tovec3(dsubvec3(world, cam));
}

static mat4 camRelTranslate(dvec3 world, dvec3 cam) {
	return translatevec3(toCamRel(world, cam));
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
	return perspectiveReversedInf((float)CAMERA_FOV_Y_DEG, Window_Width()/Window_Height(), CAMERA_NEAR);
}

/* Main camera projection * view (FrameContext.viewProj); the quadtree
   walks frustum-cull their nodes against it (with the infinite projection
   its far plane never culls). */
static mat4 getViewProjection(void) {
	return multiplymat4(cameraProjection(), Camera_ViewMatrix());
}

void Render_Frame(App *app)
{
	dvec3 cam = app->cam;
	int activeId = SolarSystem_ActiveBody();
	int isEarth = activeId == BODY_EARTH;
	const Body *planetB = Bodies_Get(activeId);
	const Body *sun = Bodies_Get(BODY_SUN);
	const Body *moon = Bodies_Get(BODY_MOON);
	mat4 model;

	/* Full passes for the active planet and the Moon only while they are
	   more than a few pixels across; Planets_Draw stands in otherwise.
	   The Moon needs Earth's shadow map, so only with Earth active. */
	int planetFull = Planets_FullDetail(planetB, cam);
	int moonFull = isEarth && Planets_FullDetail(moon, cam);

	/* Planet radius (metres) and world centre. */
	float fScale = (float)planetB->radius;
	dvec3 translation = planetB->position;

	/* Atmosphere: shell starts at the planet surface; the rest of its
	   parameters live in Atmosphere_Params() (atmosphere.c, debug panel).
	   Shared by the dome (in-scatter) and the planet/water passes
	   (extinction). */
	float atmoInner = fScale;

	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
	glClearColor(0.0, 0.0, 0.0, 1.0);

	//Light calculations per frame: the sun body is the light
	dvec3 lightWorld = sun->position;
	/* Shading and shadows both use the camera-relative light. */
	vec3 lightPositionXYZ = toCamRel(lightWorld, cam);
	vec4 lightPosition = {lightPositionXYZ.x, lightPositionXYZ.y, lightPositionXYZ.z, 1.0};

	/* What every pass of this frame shares (engine/gfx/frame.h). */
	FrameContext frame;
	frame.cam = cam;
	frame.proj = cameraProjection();
	frame.view = Camera_ViewMatrix();
	frame.viewProj = getViewProjection();
	frame.nearPlane = CAMERA_NEAR;
	frame.viewportH = Window_Height();
	frame.lightPos = lightPosition;
	frame.time = App_Time();

	BodyDraw_SunMarker(app->quadCubeVAO, app->quadCubeVerts, app->sunNoiseTex, sun, &frame);

	/* Rings with the full passes this frame (the full-detail planet's, or
	   the ringed body nearest on screen: a gas giant's too) and their frame
	   for this camera + light; the receivers take their shadow only when
	   that is the planet they draw. Ring-shadow receivers that are not
	   bound by their own draw calls bind here. */
	int ringId = SolarSystem_RingBody(cam, planetFull ? activeId : -1);
	const Body *ringB = ringId >= 0 ? Bodies_Get(ringId) : planetB;
	Rings_SetPlanet(ringId >= 0, ringId >= 0 ? SolarSystem_Info(ringId)->ringSeed : 0,
	                SolarSystem_RingNormal(ringId >= 0 ? ringId : activeId));
	Rings_SetFrame(ringB->position, cam, (float)ringB->radius, lightPosition, ringId == activeId);
	Rings_BindReceiver(BodyDraw_Program());
	Rings_BindReceiver(Atmosphere_Program());

	/* Planet model (scale * spin/tilt, camera-relative), composed in
	   double and downcast once; shared by the shadow casters and the
	   main pass. */
	QuadTree_Body planetBody = TerrainDraw_PlanetBody(planetB, cam);
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
		double boundR = fScale * (1.0 + TerrainGen_Style()->landRelief) * SHADOW_BOUND_MARGIN;
		/* Earth's Moon both casts and receives; other planets only self-shadow. */
		double reach = (isEarth ? dlengthvec3(dsubvec3(moon->position, translation)) + moon->radius * 1.5 : 0.0) + boundR;
		dvec3 eye = daddvec3(translation, dscalarvec3(sunDir, reach));
		Shadows_FitGlobal(eye, translation, cam, boundR, reach);
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
			model = dmat4tomat4(Body_ModelMatrix(moon, cam));
			if (isEarth)
				Shadows_DrawMeshCaster(app->quadCubeVAO, app->quadCubeVerts, model, Shadows_GlobalLightSpace());
			/* Coarse walk, no finer than half a texel: smaller detail cannot
			   show in this map. */
			TerrainDraw_ShadowCasters(&planetBody, Shadows_GlobalLightSpace(),
			                          0.5 * Shadows_GlobalTexel(), QT_SHADOW_TRIANGLE_PX, 1, &frame);

			/* Cascades: finer maps around the camera (shadows.c), fitted
			   between the nearest and farthest terrain in view. Casters use
			   the main pass's LOD, so they are the very mesh that receives
			   (minus what each box culls), and keep everything up-sun of the
			   box (clamped onto its near plane). */
			{
				double nearest, farthest;
				TerrainDraw_ViewRange(planetB, cam, &nearest, &farthest);
				mat4 view = Camera_ViewMatrix();
				dvec3 forward = { -view.m[2][0], -view.m[2][1], -view.m[2][2] };
				dvec3 toSun = dsubvec3(lightWorld, translation);
				Shadows_Fit(cam, forward, dscalarvec3(toSun, 1.0 / dlengthvec3(toSun)), nearest, farthest);
			}
			for (int i = 0; i < Shadows_Cascades()->count; i++) {
				const ShadowCascade *sc = &Shadows_Cascades()->c[i];
				Shadows_BeginCascade(i);
				if (isEarth)
					Shadows_DrawMeshCaster(app->quadCubeVAO, app->quadCubeVerts, model, sc->lightSpace);
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

	int sceneW = (int)Window_Width(), sceneH = (int)Window_Height();
	ensureSceneTarget(sceneW, sceneH);
	glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFBO);
	glViewport(0, 0, sceneW, sceneH);
	setReversedDepth(1);

	glClearColor(0.1, 0.0, 0.2, 1.0);
	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

	//Skybox: full-screen background, drawn first (far plane, no depth write)
	mat4 skyboxProj = cameraProjection();
	Skybox_Draw(Nebula_SkyTexture(app->skyboxTex), skyboxProj, Camera_ViewRotation());

	glPolygonMode(GL_FRONT_AND_BACK, Input_Wireframe() ? GL_LINE : GL_FILL);
	if (moonFull) {
		Prof_Begin(PROF_MOON);
		BodyDraw_Moon(app->moonTex, moon, &frame);
		Prof_End(PROF_MOON);
	}
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	//Sun (billboard): opaque disc drawn with the scene; glow drawn last (below)
	mat4 sunProj = cameraProjection();
	mat4 sunView = Camera_ViewMatrix();
	float sunGlowRadius = (float)(sun->radius * SUN_GLOW_RADII);
	Sun_DrawDisc(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

	//Distant bodies: every planet not drawn in full this frame (gas
	//giants always; terrain planets and the Moon while small or
	//inactive).
	Planets_Draw(sunProj, sunView, cam, planetFull ? activeId : -1, moonFull, ringId);

	//Quadtree planet: LOD sphere built from six instanced face grids,
	//centred camera-relative (the walk runs with the camera at the origin).
	glPolygonMode(GL_FRONT_AND_BACK, Input_Wireframe() ? GL_LINE : GL_FILL);

	//Planet (land + seafloor), then the water shell over it (Earth
	//only for now), alpha-blended over the seafloor.
	if (planetFull) {
		Prof_Begin(PROF_PLANET);
		TerrainDraw_Planet(&planetBody, &frame, atmoInner, activeId, app->dxWaveTex);
		Prof_End(PROF_PLANET);
	}
	if (planetFull && isEarth) {
		Prof_Begin(PROF_WATER);
		WaterDraw_Draw(&app->ocean, planetB, &frame, app->skyboxTex, atmoInner, activeId);
		Prof_End(PROF_WATER);
	}
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	//Clouds: the layer as a slab over the planet (clouds.c), blended over
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
		Clouds_Draw(sunProj, sunView, cam, activeId);
		Prof_End(PROF_CLOUDS);
	}

	//Rings: opaque close-up rocks, then the analytic sheet and the dust
	//(premultiplied, depth-tested, no depth write). Before the
	//atmosphere dome, which takes the sheet's coverage out of its glow.
	//Whichever body they ring, stand-in or not.
	if (ringId >= 0) {
		Prof_Begin(PROF_RINGS);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   /* the cloud cover (above) */
		Rings_DrawRocks(sunProj, sunView);
		Rings_DrawSheet(sunProj, sunView);
		Rings_DrawDust(sunProj, sunView);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		Prof_End(PROF_RINGS);
	}

	//Nebula: the in-system wisps (nebula.c), marched to the scene
	//depth and blended over everything drawn so far; the dome adds
	//its air in front afterwards. Not tied to planetFull: deep space
	//is where it matters most. The alpha channel keeps the cloud
	//cover (above), as for the rings.
	if (Nebula_Enabled()) {
		Prof_Begin(PROF_NEBULA);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		Nebula_Draw(sunProj, sunView, cam);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		Prof_End(PROF_NEBULA);
	}

	//Atmosphere dome: OpenGLSolar port (GPU Gems ch.16 scattering on the
	//planet shell; params above with the planet extinction). Rays end at
	//the scene depth, so this runs after everything opaque.
	if (planetFull && Atmosphere_Params()->shellHeight > 0.0f) {
		Prof_Begin(PROF_ATMOSPHERE);
		Atmosphere_DrawDome(sunProj, sunView, camRelTranslate(translation, cam),
		                   atmoInner, Atmosphere_Params(), lightPosition);
		Prof_End(PROF_ATMOSPHERE);
	}

	//Sun corona halo — must be drawn LAST (additive, does not write depth)
	Sun_DrawGlow(sunProj, sunView, lightPositionXYZ, sunGlowRadius);

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
		glfwGetWindowSize(app->window, &ww, &wh);
		CameraSliders_Layout(ww, wh);
	}
	Bench_MaybeCapture();
	UI_DrawAll();

	//Debug panel (F1): drawn last, over the sliders.
	DebugGUI_Frame();
	Prof_End(PROF_OVERLAY);
	Prof_EndFrame();
}
