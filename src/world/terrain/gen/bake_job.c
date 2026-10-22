/* The bake pipeline (blocking and background jobs), its public API and
   the bake settings. */
#include "world/terrain/gen/gen_internal.h"

/* Sea level + ocean depth used by quad.frag/water.frag (see terrain_gen.h). */
PlanetGenParams g_planetGen = { 0.5f, 0.22f, 0 };

/* Bake inputs (terrain_gen.h). Shaping defaults mirror the [default]
   notes in heightbake.frag. */
static TerrainBakeSettings g_terrainBake = {
	.seed = 1337u,
	.plateCount = 14,
	.continentArea = 0.42f,
	.coastFreq = 2.2f,
	.coastAmt = 0.12f,
	.plainsH = 0.06f,
	.hillAmt = 0.10f,
	.plateauH = 0.30f,
	.collideH = 1.30f,
	.andesH = 0.95f,
	.oldRangeH = 0.35f,
	.oceanBase = -0.70f,
	.ridgeH = 0.28f,
	.hotspotLo = 0.50f,
	.fragLo = 0.42f,
	.detailAmt = 0.06f,
	.thermalPasses = 6,
	.thermalTalus = 0.006f,    /* only the steepest crests/cliffs exceed it */
	.dropletsPerPx = 0.35f,
	.dropletErosion = 0.3f,
	.dropletDeposit = 0.3f,
	.dropletCapacity = 4.0f,
	.dropletEvap = 0.02f,
	.dropletInertia = 0.05f,
};

/* Debug: when TERRAIN_DUMP names a file, write the final normalized field
   there as raw float32 (the six N x N faces back to back, GL face order and
   orientation) for offline inspection. TERRAIN_DUMP_CLIMATE likewise. */
static void debugDump(const float *h, size_t n)
{
	const char *path = getenv("TERRAIN_DUMP");
	if (!path || !*path) return;
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "[terrain] cannot write dump %s\n", path); return; }
	fwrite(h, sizeof(float), n, f);
	fclose(f);
}

/* ---------------- Bake pipeline ----------------
   Stages: shaping (the GPU pass and its readback), the CPU work (erosion,
   normalization, climate / ocean pixels, bounds pyramid) and the upload.
   A blocking bake runs them back to back. A background bake spreads the
   GPU pass over frames in horizontal strips, reads it back through a
   pixel buffer once a fence says it is done, runs the CPU work on a
   worker thread and uploads when polled after that: each frame only pays
   a strip, a copy or one texture upload. The CPU stage's scratch state (xsState, the
   histograms, the drainage queue) and the shaping target are shared by gen/,
   so one bake runs at a time: a new one first completes the background
   job's GPU stages and waits for its worker. */
#define BAKE_SHAPE_STRIPS 32   /* background shaping: one strip per poll */

typedef enum { STAGE_SHAPE, STAGE_READBACK, STAGE_CPU, STAGE_UPLOAD } BakeStage;

struct TerrainBakeJob {
	TerrainBakeSettings bs;
	PlanetStyle style;            /* the baked planet's (not the active one's) */
	void (*step)(void);
	BakeStage stage;
	int strip;                    /* next strip to draw (STAGE_SHAPE) */
	GLuint pbo;                   /* readback buffer (STAGE_READBACK) */
	GLsync fence;
	float *rg;                    /* shaping readback (W x H, R = height, G = anomaly); the CPU stage frees it */
	/* CPU stage results, uploaded by the finish */
	TerrainPyramid *pyramid;      /* level 0 is the final field */
	unsigned char *climatePx, *oceanPx;
	unsigned short *surfacePx;    /* half floats */
	float seaLevel, oceanDepth, landStepRms;
	/* Upload results (STAGE_UPLOAD: one per poll) */
	GLuint climateTex, oceanTex, surfaceTex, heightTex;
	double uploadS;
	clock_t t[7];
	int background;
	HANDLE thread;                /* the worker, until joined */
	volatile LONG done;           /* CPU stage finished */
};

static TerrainBakeJob *bgJob;       /* the background job, until finished or cancelled */
static volatile LONG workerSteps;   /* its progress, in step() calls */

static void workerStep(void)
{
	InterlockedIncrement(&workerSteps);
}

/* Draws shaping strips [job->strip, end) into the working target. The
   shader's uniforms (seed, plates, shaping) were set by the first call and
   stay in the program between frames. */
static void shapeStrips(TerrainBakeJob *job, int end)
{
	const int W = FACE_N, H = 6 * FACE_N;
	if (job->strip == 0) {
		ensureBakeResources();
		job->t[0] = clock();
		glUseProgram(bakeProgram);
		glUniform1f(glGetUniformLocation(bakeProgram, "seed"), (float)job->bs.seed);
		glUniform1i(glGetUniformLocation(bakeProgram, "faceSize"), FACE_N);
		uploadPlates(&job->bs);
		uploadShaping(&job->bs);
	}
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glViewport(0, 0, W, H);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glUseProgram(bakeProgram);
	glBindVertexArray(bakeQuadVAO);
	for (; job->strip < end; job->strip++) {
		int y0 = H * job->strip / BAKE_SHAPE_STRIPS, y1 = H * (job->strip + 1) / BAKE_SHAPE_STRIPS;
		glScissor(0, y0, W, y1 - y0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, Window_Width(), Window_Height());
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	if (blend) glEnable(GL_BLEND);
	if (!scissor) glDisable(GL_SCISSOR_TEST);
}

#define READBACK_BYTES (CUBE_TEXELS * 2 * sizeof(float))

static float *allocRG(void)
{
	float *p = malloc(READBACK_BYTES);
	if (!p) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	return p;
}

/* Blocking readback of the whole target into job->rg (only the two
   channels the bake uses: half the copy). */
static void readbackNow(TerrainBakeJob *job)
{
	job->rg = allocRG();
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, FACE_N, 6 * FACE_N, GL_RG, GL_FLOAT, job->rg);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	job->t[1] = clock();
}

/* Starts an asynchronous readback into a pixel buffer, fenced. */
static void readbackStart(TerrainBakeJob *job)
{
	glGenBuffers(1, &job->pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, job->pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)READBACK_BYTES, NULL, GL_STREAM_READ);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, FACE_N, 6 * FACE_N, GL_RG, GL_FLOAT, (void *)0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	job->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
}

static void readbackRelease(TerrainBakeJob *job)
{
	if (job->fence) glDeleteSync(job->fence);
	if (job->pbo) glDeleteBuffers(1, &job->pbo);
	job->fence = 0;
	job->pbo = 0;
}

/* Copies a finished pixel-buffer readback into job->rg. wait: block for
   the fence. Returns 1 once copied. */
static int readbackCollect(TerrainBakeJob *job, int wait)
{
	GLenum r = glClientWaitSync(job->fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
	                            wait ? 10000000000ull : 0);
	if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
		return 0;
	job->rg = allocRG();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, job->pbo);
	const void *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)READBACK_BYTES, GL_MAP_READ_BIT);
	if (src)
		memcpy(job->rg, src, READBACK_BYTES);
	else
		memset(job->rg, 0, READBACK_BYTES);
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	readbackRelease(job);
	job->t[1] = clock();
	return 1;
}

/* 2-7. Everything between the readback and the upload. No GL. */
static void bakeCPU(TerrainBakeJob *job)
{
	const TerrainBakeSettings *bs = &job->bs;
	const size_t n = CUBE_TEXELS;
	static Histo hf;   /* 32 KB: keep it off the stack */

	xsState = bs->seed * 2654435761u | 1u;

	/* 2. Split the readback (r = shaped h, g = climate anomaly). */
	float *h = malloc(n * sizeof(float));
	float *anom = malloc(n * sizeof(float));
	float *dep = calloc(n, sizeof(float));
	if (!h || !anom || !dep) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	for (size_t i = 0; i < n; i++) { h[i] = job->rg[i * 2]; anom[i] = job->rg[i * 2 + 1]; }
	free(job->rg);
	job->rg = NULL;
	clampFinite(h, n);
	clampFinite(anom, n);

	/* Pre-normalize so the erosion constants are in fixed [0,1] units. The
	   shaper's sea level is raw 0; track where it lands. */
	histoBuildCube(h, &hf);
	float preLo = histoPctl(&hf, PRE_LO_PCT), preHi = histoPctl(&hf, PRE_HI_PCT);
	stretch01(h, n, preLo, preHi);
	float seaLevel0 = (0.0f - preLo) / fmaxf(preHi - preLo, 1e-6f);
	job->t[2] = clock();

	/* 3. Thermal erosion (weathering), then 4. hydraulic droplet erosion. */
	thermalErode(h, bs->thermalPasses, bs->thermalTalus, job->step);
	job->t[3] = clock();

	/* Droplets spawn only on land; reaching the sea is a river mouth. */
	hydraulicErode(h, dep, seaLevel0, bs, job->step);
	job->t[4] = clock();

	/* 5. Percentile-stretch the relief to [0,1], carrying the sea level
	   (raw 0, set by the plate crust) through it, + ocean depth (95th area
	   percentile of depth below sea level, feeds the water.frag tint). */
	histoBuildCube(h, &hf);
	float lo = histoPctl(&hf, NORMALIZE_LO_PCT);
	float hi = histoPctl(&hf, NORMALIZE_HI_PCT);
	float range = hi - lo;
	if (range < 1e-6f) range = 1.0f;
	float seaLevel = (seaLevel0 - lo) / range;
	seaLevel = seaLevel < 0.0f ? 0.0f : (seaLevel > 1.0f ? 1.0f : seaLevel);
	stretch01(h, n, lo, hi);

	/* Area-weighted depth histogram over the ocean texels: depths lie in
	   (0, seaLevel]. */
	static double dhist[PCTL_BINS];
	memset(dhist, 0, sizeof dhist);
	double oceanArea = 0.0;
	float oceanDepth = 0.15f;
	if (seaLevel > 1e-6f) {
		double dscale = (double)(PCTL_BINS - 1) / (double)seaLevel;
		for (size_t i = 0; i < n; i++) {
			if (h[i] < seaLevel) {
				double w = texArea[i % FACE_TEXELS];
				oceanArea += w;
				int b = (int)((double)(seaLevel - h[i]) * dscale);
				dhist[b < 0 ? 0 : (b > PCTL_BINS - 1 ? PCTL_BINS - 1 : b)] += w;
			}
		}
		if (oceanArea > 0.0) {
			double target = OCEAN_DEPTH_PCT * oceanArea;
			double acc = 0.0;
			int bin = PCTL_BINS - 1;
			for (int b = 0; b < PCTL_BINS; b++) {
				acc += dhist[b];
				if (acc > target) { bin = b; break; }
			}
			oceanDepth = ((float)bin + 0.5f) / (float)dscale;
		}
	}
	if (!(oceanDepth > 1e-3f)) oceanDepth = 1e-3f;

	debugDump(h, n);
	job->t[5] = clock();

	/* 7. Climate map (moisture, rivers, alluvium, lakes) and ocean map. The
	   latitude streams (moisture, fetch) run on an equirect copy. */
	float *hEq = cubeToEquirect(h), *anomEq = cubeToEquirect(anom);
	job->climatePx = climatePixels(&job->style, h, hEq, anomEq, dep, seaLevel);
	job->oceanPx = oceanPixels(hEq, anom, seaLevel);
	job->surfacePx = surfacePixels(&job->style, h, seaLevel, oceanDepth, &job->landStepRms);
	free(hEq);
	free(anomEq);
	free(anom);
	free(dep);
	job->seaLevel = seaLevel;
	job->oceanDepth = oceanDepth;
	/* Keeps h as level 0 of the quadtree's height-bounds pyramid. */
	job->pyramid = buildPyramid(h);
	job->t[6] = clock();
}

static unsigned __stdcall bakeWorker(void *arg)
{
	TerrainBakeJob *job = arg;
	bakeCPU(job);
	InterlockedExchange(&job->done, 1);
	return 0;
}

/* The CPU stage of a background job: on a worker thread, below the render
   thread's priority. */
static void startWorker(TerrainBakeJob *job)
{
	job->stage = STAGE_CPU;
	job->thread = (HANDLE)_beginthreadex(NULL, 0, bakeWorker, job, CREATE_SUSPENDED, NULL);
	if (job->thread) {
		SetThreadPriority(job->thread, THREAD_PRIORITY_BELOW_NORMAL);
		ResumeThread(job->thread);
	} else {
		/* No thread: bake it here instead. */
		bakeCPU(job);
		job->done = 1;
	}
}

/* Advances a background job's main-thread stages; wait: run them all now. */
static void advanceJob(TerrainBakeJob *job, int wait)
{
	if (job->stage == STAGE_SHAPE) {
		shapeStrips(job, wait ? BAKE_SHAPE_STRIPS : job->strip + 1);
		if (job->strip < BAKE_SHAPE_STRIPS)
			return;
		readbackStart(job);
		job->stage = STAGE_READBACK;
		if (!wait)
			return;   /* collect it next time: the GPU is still busy */
	}
	if (job->stage == STAGE_READBACK) {
		if (!readbackCollect(job, wait))
			return;
		startWorker(job);
	}
}

/* Runs a background job to the end of its CPU stage. */
static void completeJob(TerrainBakeJob *job)
{
	advanceJob(job, 1);
	if (job->thread) {
		WaitForSingleObject(job->thread, INFINITE);
		CloseHandle(job->thread);
		job->thread = NULL;
	}
}

static TerrainBakeJob *newJob(const TerrainBakeSettings *bs, const PlanetStyle *style,
                              void (*step)(void))
{
	if (bgJob)
		completeJob(bgJob);   /* one bake at a time */
	TerrainBakeJob *job = calloc(1, sizeof *job);
	if (!job) {
		fprintf(stderr, "[terrain] out of memory during heightmap bake\n");
		exit(1);
	}
	job->bs = *bs;
	job->style = *style;
	job->step = step;
	return job;
}

static void freeJob(TerrainBakeJob *job)
{
	if (bgJob == job)
		bgJob = NULL;
	readbackRelease(job);
	free(job->rg);
	free(job->climatePx);
	free(job->oceanPx);
	free(job->surfacePx);
	freePyramid(job->pyramid);
	GLuint tex[4] = { job->climateTex, job->oceanTex, job->surfaceTex, job->heightTex };
	for (int k = 0; k < 4; k++)
		if (tex[k]) glDeleteTextures(1, &tex[k]);
	free(job);
}

/* 8. Upload: climate / ocean / surface maps and the final field (the same
   sampling params the body shaders expect) with full mip chains, one per
   call (all: the rest at once). R32F: half floats would round heights to ~100 m
   steps at Earth scale, and the CPU bounds pyramid must see exactly the
   values the GPU samples. Returns 1 when all three are up. */
static int uploadNext(TerrainBakeJob *job, int all)
{
	job->stage = STAGE_UPLOAD;
	do {
		clock_t u0 = clock();
		if (!job->climateTex) {
			job->climateTex = uploadRGBA8(job->climatePx);
			free(job->climatePx);
			job->climatePx = NULL;
		} else if (!job->oceanTex) {
			job->oceanTex = uploadRGBA8(job->oceanPx);
			free(job->oceanPx);
			job->oceanPx = NULL;
		} else if (!job->surfaceTex) {
			job->surfaceTex = uploadCube(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, job->surfacePx,
			                             FACE_TEXELS * 4 * sizeof(unsigned short));
			free(job->surfacePx);
			job->surfacePx = NULL;
		} else if (!job->heightTex) {
			job->heightTex = uploadHeight(job->pyramid->mn[0]);
		}
		job->uploadS += (double)(clock() - u0) / CLOCKS_PER_SEC;
	} while (all && !job->heightTex);
	return job->heightTex != 0;
}

/* Hands the uploaded planet to out (the pyramid moves there) and frees the
   job. Returns the height texture. */
static GLuint bakeFinish(TerrainBakeJob *job, PlanetGenParams *out)
{
	uploadNext(job, 1);
	out->climateTex = job->climateTex;
	out->oceanTex = job->oceanTex;
	out->surfaceTex = job->surfaceTex;
	out->seaLevel = job->seaLevel;
	out->oceanDepth = job->oceanDepth;
	out->landStepRms = job->landStepRms;
	out->pyramid = job->pyramid;
	job->pyramid = NULL;
	GLuint heightTex = job->heightTex;
	job->climateTex = job->oceanTex = job->surfaceTex = job->heightTex = 0;

	const clock_t *t = job->t;
	double s = (double)(CLOCKS_PER_SEC);
	fprintf(stderr, "[terrain] bake seed %u%s: shape+readback %.3f s, split %.2f s, thermal %.2f s, hydro %.2f s, sea %.2f s, climate %.2f s, upload %.3f s -> seaLevel %.4f, oceanDepth %.4f, land step %.5f\n",
	        job->bs.seed, job->background ? " (background)" : "",
	        (double)(t[1] - t[0]) / s, (double)(t[2] - t[1]) / s, (double)(t[3] - t[2]) / s,
	        (double)(t[4] - t[3]) / s, (double)(t[5] - t[4]) / s, (double)(t[6] - t[5]) / s,
	        job->uploadS, job->seaLevel, job->oceanDepth, job->landStepRms);

	freeJob(job);
	return heightTex;
}

unsigned int TerrainGen_BakeHeightmap(const TerrainBakeSettings *bs, const PlanetStyle *style,
                                     PlanetGenParams *out, void (*step)(void))
{
	TerrainBakeJob *job = newJob(bs, style, step);
	shapeStrips(job, BAKE_SHAPE_STRIPS);
	readbackNow(job);
	bakeCPU(job);
	GLuint heightTex = bakeFinish(job, out);
	/* The new planet becomes the active one. */
	g_planetGen = *out;
	return heightTex;
}

TerrainBakeJob *TerrainGen_BakeStart(const TerrainBakeSettings *bs, const PlanetStyle *style)
{
	TerrainBakeJob *job = newJob(bs, style, workerStep);
	job->background = 1;
	InterlockedExchange(&workerSteps, 0);
	bgJob = job;
	advanceJob(job, 0);
	return job;
}

int TerrainGen_BakePoll(TerrainBakeJob *job)
{
	if (job->stage < STAGE_CPU)
		advanceJob(job, 0);
	else if (job->done) {
		if (job->thread) {
			CloseHandle(job->thread);   /* finished: nothing to wait for */
			job->thread = NULL;
		}
		return uploadNext(job, 0);
	}
	return 0;
}

float TerrainGen_BakeProgress(const TerrainBakeJob *job)
{
	/* Shaping ~5%; the steps cover the erosion, the climate maps one more. */
	if (job->stage < STAGE_CPU)
		return 0.05f * (float)job->strip / (float)BAKE_SHAPE_STRIPS;
	if (job->done)
		return 1.0f;
	float p = 0.05f + 0.95f * (float)workerSteps / (float)(TERRAIN_BAKE_STEPS + 1);
	return p < 1.0f ? p : 1.0f;
}

unsigned int TerrainGen_BakeFinish(TerrainBakeJob *job, PlanetGenParams *out)
{
	completeJob(job);
	return bakeFinish(job, out);
}

void TerrainGen_BakeCancel(TerrainBakeJob *job)
{
	if (job->thread) {
		WaitForSingleObject(job->thread, INFINITE);
		CloseHandle(job->thread);
		job->thread = NULL;
	}
	freeJob(job);
}

PlanetGenParams *TerrainGen_GenParams(void)
{
	return &g_planetGen;
}

PlanetStyle *TerrainGen_Style(void)
{
	return &g_planetStyle;
}

TerrainBakeSettings *TerrainGen_BakeSettings(void)
{
	return &g_terrainBake;
}
