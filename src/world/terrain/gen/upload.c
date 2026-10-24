/* GPU shaping resources, tectonic plates and shaping uniforms, and the
   cube map uploads. */
#include "world/terrain/gen/gen_internal.h"

/* ---------------- GPU shaping (cached across re-bakes) ---------------- */

unsigned int bakeProgram, bakeQuadVAO, workFBO, workTex;

void ensureBakeResources(void)
{
	if (bakeProgram) return;

	Shader_Create(&bakeProgram, "world/terrain/heightbake.vert", "world/terrain/heightbake.frag");

	static const float quadCoords[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	glGenVertexArrays(1, &bakeQuadVAO);
	glBindVertexArray(bakeQuadVAO);
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadCoords), quadCoords, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	/* Working RGBA16F target, read back as GL_RG / GL_FLOAT (the two
	   channels the bake uses): the six faces stacked bottom to top, so a
	   readback row is face * N + j, the CPU field layout. */
	glGenTextures(1, &workTex);
	glBindTexture(GL_TEXTURE_2D, workTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, FACE_N, 6 * FACE_N, 0,
	             GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &workFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, workTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[terrain] working framebuffer incomplete: 0x%x\n",
		        (unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER));
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	initRowCos(EQ_H);
	initCubeMetric();
	initBrush();
}

/* Uploads an RGBA8 map (climate / ocean layout) with its mip chain. GL:
   main thread only. */
/* Uploads a cube map (faces back to back, faceBytes each) with its mip
   chain. Seamless filtering is switched on for good: the body shaders
   sample across face edges. GL: main thread only. */
GLuint uploadCube(GLenum internalFormat, GLenum format, GLenum type,
                  const void *px, size_t faceBytes)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (int f = 0; f < 6; f++)
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, internalFormat, FACE_N, FACE_N, 0,
		             format, type, (const unsigned char *)px + f * faceBytes);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, workFBO);
	glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	return tex;
}

GLuint uploadRGBA8(const unsigned char *px)
{
	return uploadCube(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, px, FACE_TEXELS * 4);
}

/* ---------------- Tectonic plates ---------------- */

/* Plate centres (min-separated random points), continental flags (random
   plates until continentArea of the sphere, by Monte-Carlo cell area),
   rotation poles/speeds and passive-margin widths, uploaded to the shaping
   shader. Own PRNG so the erosion stream is unaffected. */
static unsigned int plRng;
static float plRand(void)
{
	plRng ^= plRng << 13; plRng ^= plRng >> 17; plRng ^= plRng << 5;
	return (float)(plRng >> 8) * (1.0f / 16777216.0f);
}
static void plRandDir(float v[3])
{
	float z = 2.0f * plRand() - 1.0f, a = 6.2831853f * plRand();
	float r = sqrtf(fmaxf(1.0f - z * z, 0.0f));
	v[0] = r * cosf(a); v[1] = r * sinf(a); v[2] = z;
}

void uploadPlates(const TerrainBakeSettings *bs)
{
	float c[TERRAIN_PLATE_MAX][4], mo[TERRAIN_PLATE_MAX][4];
	int np = 0;
	int plateCount = bs->plateCount < 2 ? 2
	               : (bs->plateCount > TERRAIN_PLATE_MAX ? TERRAIN_PLATE_MAX : bs->plateCount);
	plRng = bs->seed * 747796405u + 2891336453u;
	if (!plRng) plRng = 1u;
	float minDot = cosf((float)PLATE_MIN_SEP);
	for (int tries = 0; np < plateCount && tries < 10000; tries++) {
		float v[3];
		plRandDir(v);
		int ok = 1;
		for (int k = 0; k < np && ok; k++)
			if (v[0] * c[k][0] + v[1] * c[k][1] + v[2] * c[k][2] > minDot) ok = 0;
		if (!ok) continue;
		c[np][0] = v[0]; c[np][1] = v[1]; c[np][2] = v[2]; c[np][3] = 0.0f;
		np++;
	}

	/* Cell areas (unwarped Voronoi) by uniform sampling. */
	int count[TERRAIN_PLATE_MAX] = { 0 };
	const int samples = 8192;
	for (int s = 0; s < samples; s++) {
		float v[3];
		plRandDir(v);
		int best = 0; float bd = -2.0f;
		for (int k = 0; k < np; k++) {
			float d = v[0] * c[k][0] + v[1] * c[k][1] + v[2] * c[k][2];
			if (d > bd) { bd = d; best = k; }
		}
		count[best]++;
	}
	/* Random order; mark continental until the area target (min 2). */
	int order[TERRAIN_PLATE_MAX];
	for (int k = 0; k < np; k++) order[k] = k;
	for (int k = np - 1; k > 0; k--) {
		int j = (int)(plRand() * (float)(k + 1)); if (j > k) j = k;
		int t = order[k]; order[k] = order[j]; order[j] = t;
	}
	float area = 0.0f;
	for (int k = 0, nc = 0; k < np; k++) {
		if (nc >= 2 && area >= bs->continentArea) break;
		c[order[k]][3] = 1.0f;
		area += (float)count[order[k]] / (float)samples;
		nc++;
	}

	for (int k = 0; k < np; k++) {
		float ax[3];
		plRandDir(ax);
		float speed = 0.5f + plRand();
		mo[k][0] = ax[0] * speed; mo[k][1] = ax[1] * speed; mo[k][2] = ax[2] * speed;
		mo[k][3] = plRand() * PLATE_MARGIN_MAX;
	}
	glUniform1i(glGetUniformLocation(bakeProgram, "plateCount"), np);
	glUniform4fv(glGetUniformLocation(bakeProgram, "plates"), np, &c[0][0]);
	glUniform4fv(glGetUniformLocation(bakeProgram, "plateMotion"), np, &mo[0][0]);
}

/* Octaves of noise from wavelength lambda (m) down to two map texels at
   the face centre (at least one). */
static int octavesDownTo(double lambda, double radius)
{
	double texel = 2.0 * radius / FACE_N;
	int n = (int)floor(log2(lambda / (2.0 * texel))) + 1;
	return n < 1 ? 1 : (n > 10 ? 10 : n);
}

/* A ground-scale wavelength (m) as heightbake.frag's noise frequency: its
   coordinates are the unit sphere's, so features ~1 / frequency radii. */
static void uniformWave(const char *freq, const char *oct, double lambda, double radius)
{
	glUniform1f(glGetUniformLocation(bakeProgram, freq), (float)(radius / lambda));
	glUniform1i(glGetUniformLocation(bakeProgram, oct), octavesDownTo(lambda, radius));
}

/* Shaping recipe uniforms (heightbake.frag keeps the #define names). */
void uploadShaping(const TerrainBakeSettings *bs)
{
	double R = bs->radiusM > 0.0f ? bs->radiusM : EARTH_RADIUS_M;
	glUniform1f(glGetUniformLocation(bakeProgram, "COLLIDE_WIDTH"), (float)(TERRAIN_COLLIDE_WIDTH_M / R));
	glUniform1f(glGetUniformLocation(bakeProgram, "ANDES_WIDTH"), (float)(TERRAIN_ANDES_WIDTH_M / R));
	glUniform1f(glGetUniformLocation(bakeProgram, "TIBET_WIDTH"), (float)(TERRAIN_TIBET_WIDTH_M / R));
	uniformWave("FREQ_MOUNT", "OCT_MOUNT", TERRAIN_MOUNT_WAVELENGTH_M, R);
	glUniform1f(glGetUniformLocation(bakeProgram, "GAIN_MOUNT"), (float)TERRAIN_MOUNT_GAIN);
	uniformWave("FREQ_OLD", "OCT_OLD", TERRAIN_OLD_RANGE_WAVELENGTH_M, R);
	uniformWave("FREQ_HILL", "OCT_HILL", TERRAIN_HILL_WAVELENGTH_M, R);
	uniformWave("FREQ_ROUGH", "OCT_ROUGH", TERRAIN_ROUGH_MAP_WAVELENGTH_M, R);
	glUniform1f(glGetUniformLocation(bakeProgram, "ROUGH_L0"), (float)(TERRAIN_ROUGH_MAP_WAVELENGTH_M / 10000.0));
	glUniform2f(glGetUniformLocation(bakeProgram, "ROUGH_A10"),
	            (float)(TERRAIN_ROUGH_A10_PLAINS_M / 1000.0), (float)(TERRAIN_ROUGH_A10_MOUNTAIN_M / 1000.0));
	glUniform2f(glGetUniformLocation(bakeProgram, "ROUGH_H"), (float)TERRAIN_ROUGH_H_PLAINS, (float)TERRAIN_ROUGH_H_MOUNTAIN);
	glUniform2f(glGetUniformLocation(bakeProgram, "ROUGH_KNEE"),
	            (float)(TERRAIN_ROUGH_KNEE_PLAINS_M / 10000.0), (float)(TERRAIN_ROUGH_KNEE_MOUNTAIN_M / 10000.0));
	glUniform1f(glGetUniformLocation(bakeProgram, "ROUGH_H_LONG"), (float)TERRAIN_ROUGH_H_LONG);
	glUniform1f(glGetUniformLocation(bakeProgram, "COAST_FREQ"), bs->coastFreq);
	glUniform1f(glGetUniformLocation(bakeProgram, "COAST_AMT"), bs->coastAmt);
	glUniform1f(glGetUniformLocation(bakeProgram, "PLAINS_H"), bs->plainsH);
	glUniform1f(glGetUniformLocation(bakeProgram, "HILL_AMT"), bs->hillAmt);
	glUniform1f(glGetUniformLocation(bakeProgram, "PLATEAU_H"), bs->plateauH);
	glUniform1f(glGetUniformLocation(bakeProgram, "COLLIDE_H"), bs->collideH);
	glUniform1f(glGetUniformLocation(bakeProgram, "ANDES_H"), bs->andesH);
	glUniform1f(glGetUniformLocation(bakeProgram, "OLD_RANGE_H"), bs->oldRangeH);
	glUniform1f(glGetUniformLocation(bakeProgram, "OCEAN_BASE"), bs->oceanBase);
	glUniform1f(glGetUniformLocation(bakeProgram, "RIDGE_H"), bs->ridgeH);
	glUniform1f(glGetUniformLocation(bakeProgram, "HOTSPOT_LO"), bs->hotspotLo);
	glUniform1f(glGetUniformLocation(bakeProgram, "FRAG_LO"), bs->fragLo);
	glUniform1f(glGetUniformLocation(bakeProgram, "DETAIL_AMT"), bs->detailAmt);
}

GLuint uploadHeight(const float *h)
{
	return uploadCube(GL_R32F, GL_RED, GL_FLOAT, h, FACE_TEXELS * sizeof(float));
}
