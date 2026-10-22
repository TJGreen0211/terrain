/*
	* Author: Tyler Green

	This module generates the FFT ocean: OCEAN_CASCADES patches of waves
	(WorldConfig.h), each simulating its own band of wavelengths.
	The initial spectrum h0(k) is drawn once on the CPU; every frame's
	spectrum, FFTs and packing run on the GPU (see the GPU section below).
*/

#include "Water.h"
#include "Common.h"
#include "ComplexMath.h"
#include "Bench.h"
#include "WorldConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void initWavesGPU(waves *water);
static void generateWavesGPU(waves *water);

static complexType complexConj(complexType c) {
	complexType cConj;
	cConj.real = c.real;
	cConj.im = -c.im;
	return cConj;
}

vec4 gaussRand() {
	float noise00 = ((float)rand() + 1.0f)/((float)RAND_MAX + 1.0f);
	float noise01 = ((float)rand() + 1.0f)/((float)RAND_MAX + 1.0f);
	float noise02 = ((float)rand() + 1.0f)/((float)RAND_MAX + 1.0f);
	float noise03 = ((float)rand() + 1.0f)/((float)RAND_MAX + 1.0f);

	float u0 = 2.0*M_PI*noise00;
	float v0 = sqrt(-2.0 * logf(noise01));
	float u1 = 2.0*M_PI*noise02;
	float v1 = sqrt(-2.0 * logf(noise03));
	vec4 grnd = {v0*cos(u0), v0 *sin(u0), v1*cos(u1), v1 * sin(u1)};
	return grnd;
}

float phillips(vec2 k) {
	float A = 4.0;
	float g = 9.81;
	vec2 waveDir = {30.0, 0.0};

	float k2 = k.x * k.x + k.y * k.y;
	if(k2 < 0.0001) return 0.0;
	float wV = lengthvec2(waveDir);
	float L = (wV*wV)/g;
	float dampingVal = 0.0001;
	float omegaK = dotvec2(normalizevec2(waveDir),normalizevec2(k));
	//vec2 test = normalizevec2(k);
	//printf("omegaK: %f, wV: %f, L: %f\n", omegaK, wV, L);

	//	         exp(-1/(kL)^2)
	//P(k) = A * --------------- * |K . W|^2 * exp(-k^2*l^2)
	//				 k^4
	return A * (exp(-1.0/ (k2 * L * L))/(k2*k2)) * (omegaK*omegaK*omegaK*omegaK) * exp(-k2*L*L*dampingVal*dampingVal);
}

static float waveNumber(float n, int dim, float L) {
	float idx = (n < (dim / 2.0f)) ? n : n - dim;
	return 2.0f*M_PI*idx/L;
}

// Spectra are stored row-major: column M is texture u (east), row N is
// texture v (north), so kx comes from M and kz from N.
static vec2 waveVector(int N, int M, int dim, float L) {
	vec2 k = {waveNumber((float)M, dim, L), waveNumber((float)N, dim, L)};
	return k;
}

/* Wavenumber band boundaries between the cascades: cascade c keeps
   kLo <= |k| < kHi, the boundary sitting at the 4th harmonic of the next
   (smaller) patch, so that patch has a ring of bins there to carry the
   waves, while it is still well under the larger patch's Nyquist limit.
   Every wave is simulated by exactly one cascade. */
#define CASCADE_BAND_HARMONIC 4.0f

/* Initial spectrum h0(k) and its mirror conj(h0(-k)), dim*dim each, for a
   patch of side L restricted to kLo <= |k| < kHi. The amplitude carries
   the bin size 2 pi / L of the discrete spectrum (Tessendorf's
   sqrt(P(k) dk_x dk_z)), so cascades of different sizes add up to the one
   continuous spectrum. */
static void initH0(int dim, float L, float kLo, float kHi, complexType *h0k, complexType *conjH0mk) {
	float dk = 2.0f*(float)M_PI/L;
	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			vec2 k = waveVector(N, M, dim, L);
			float kl = sqrtf(k.x*k.x + k.y*k.y);

			vec4 grnd = gaussRand();
			float h0 = 1/sqrt(2.0) * sqrt(phillips(k)) * dk;
			if(kl < kLo || kl >= kHi) h0 = 0.0;
			// Nyquist bins are their own mirror: i*k*h there is not Hermitian
			// and would leak into the packed imaginary channels.
			if(N == dim/2 || M == dim/2) h0 = 0.0;
			int offset = N*dim + M;
			h0k[offset].real = grnd.x*h0; h0k[offset].im = grnd.y*h0;
		}
	}

	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			int offset = N*dim + M;
			int mirror = ((dim - N) % dim)*dim + ((dim - M) % dim);
			conjH0mk[offset] = complexConj(h0k[mirror]);
		}
	}
}

/* One layer per cascade; every map is mipmapped (refilled every frame):
   water.vert fetches height and displacement at the mesh spacing's mip,
   and at grazing views the fragment maps are minified hard (level 0 there
   thrashes the texture cache and shimmers). For the slope squares the mip
   chain is also what turns averaged-away waves into roughness. */
static GLuint createWaveArray(int dim, GLint internalFormat, GLenum format) {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
	for(int l = 0, d = dim; d > 0; l++, d >>= 1)
		glTexImage3D(GL_TEXTURE_2D_ARRAY, l, internalFormat, d, d, OCEAN_CASCADES, 0, format, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	return tex;
}

double waveCascadePatchM(int c) {
	return OCEAN_WAVE_PATCH_M / pow(OCEAN_CASCADE_RATIO, c);
}

/* Each cascade's share of the calibrated rms height (initializeWaves). */
static double s_cascadeRmsFrac[OCEAN_CASCADES];

double waveCascadeRmsFrac(int c) {
	return s_cascadeRmsFrac[c];
}

double waveCascadeScale(int c) {
	return (c == OCEAN_CASCADES - 1) ? 1.0 : fmax(g_waveParams[3], 0.01);
}

void initializeWaves(waves *water, int dim) {
	water->dimension = dim;
	for(int c = 0; c < OCEAN_CASCADES; c++)
		water->patchM[c] = (float)waveCascadePatchM(c);

	water->dyTexture = createWaveArray(dim, GL_R32F, GL_RED);
	water->dispTexture = createWaveArray(dim, GL_RG32F, GL_RG);
	water->slopeTexture = createWaveArray(dim, GL_RGBA32F, GL_RGBA);
	/* RGBA, not RGB: 3-channel float render targets and mip builds are a
	   slow driver path; alpha is unused padding. */
	water->jacobTexture = createWaveArray(dim, GL_RGBA32F, GL_RGBA);

	initWavesGPU(water);

	// Calibrate displacement once so wave height is a fixed fraction of the
	// unit sphere radius no matter the spectrum tuning (planet is drawn on the
	// unit sphere; sea level = radius 1): one update at unit scale, read the
	// raw heights of every cascade back, then scale so the rms of their sum
	// (independent bands: the variances add) hits the target. One scale for
	// all, so the cascades keep the spectrum's proportions.
	{
		int dim2 = dim*dim;
		water->heightScale = 1.0f;
		generateWavesGPU(water);
		float *raw = malloc((size_t)dim2*OCEAN_CASCADES*sizeof(float));
		glBindTexture(GL_TEXTURE_2D_ARRAY, water->dyTexture);
		glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RED, GL_FLOAT, raw);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		double var = 0.0, varC[OCEAN_CASCADES];
		for(int c = 0; c < OCEAN_CASCADES; c++) {
			double sum = 0.0;
			for(int i = 0; i < dim2; i++)
				sum += (double)raw[c*dim2 + i]*raw[c*dim2 + i];
			varC[c] = sum/(double)dim2;
			var += varC[c];
		}
		free(raw);
		for(int c = 0; c < OCEAN_CASCADES; c++)
			s_cascadeRmsFrac[c] = (var > 0.0) ? sqrt(varC[c]/var) : 0.0;
		float rms = (float)sqrt(var);
		float targetRms = OCEAN_FFT_RMS;
		// Absorbs the 1/dim^2 unnormalized-IFFT factor: data = raw * heightScale.
		water->heightScale = (rms > 0.0f) ? targetRms/rms : 1.0f;
	}
}

/* ---------------- GPU simulation ----------------
   Every frame's update as fragment-shader passes, written straight into the
   texture layers water.vert/.frag sample, so nothing crosses the bus per
   frame. Per cascade:
     wavespectrum.frag  h0 -> the four time-t spectra (two RGBA targets),
     wavefft.frag       log2(N) Stockham stages along x, then along y,
                        ping-ponging between two target pairs,
     wavepack.frag      scale + pack into that cascade's layer of the
                        dy/disp/slope/jacob arrays.
   One ocean exists, so file scope is enough. The cascades run one after
   another and share the ping-pong pairs. */
static GLuint s_specProg, s_fftProg, s_packProg;
static GLuint s_emptyVAO;
static GLuint s_h0Tex[OCEAN_CASCADES];
static GLuint s_pingTex[2][2];   /* [pair][A = hDxz|disp, B = slope|jacob] */
static GLuint s_pingFBO[2];
static GLuint s_packFBO[OCEAN_CASCADES];
static GLint s_specTime, s_specPatch, s_fftStage, s_fftHorizontal, s_packScale, s_packDScale;

static const GLenum s_drawBufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                      GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };

static GLuint createPingTexture(int dim) {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, dim, dim, 0, GL_RGBA, GL_FLOAT, NULL);
	/* Read with texelFetch only; NEAREST without mips keeps it complete. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return tex;
}

static void checkFBO(void) {
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[water] GPU wave FBO incomplete\n");
}

static GLuint createTargetFBO(const GLuint *tex, int count) {
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	for(int i = 0; i < count; i++)
		glFramebufferTexture2D(GL_FRAMEBUFFER, s_drawBufs[i], GL_TEXTURE_2D, tex[i], 0);
	glDrawBuffers(count, s_drawBufs);
	checkFBO();
	return fbo;
}

/* Level 0 of layer `layer` of each array in tex. */
static GLuint createLayerFBO(const GLuint *tex, int count, int layer) {
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	for(int i = 0; i < count; i++)
		glFramebufferTextureLayer(GL_FRAMEBUFFER, s_drawBufs[i], tex[i], 0, layer);
	glDrawBuffers(count, s_drawBufs);
	checkFBO();
	return fbo;
}

static void initWavesGPU(waves *water) {
	int dim = water->dimension;
	createShader(&s_specProg, "wavefft.vert", "wavespectrum.frag");
	createShader(&s_fftProg, "wavefft.vert", "wavefft.frag");
	createShader(&s_packProg, "wavefft.vert", "wavepack.frag");
	glGenVertexArrays(1, &s_emptyVAO);

	/* h0(k) and conj(h0(-k)) per cascade, fixed for the run (initH0). */
	complexType *h0k = malloc((size_t)dim*dim*sizeof(complexType));
	complexType *conjH0mk = malloc((size_t)dim*dim*sizeof(complexType));
	float *h0 = malloc((size_t)dim*dim*4*sizeof(float));
	for(int c = 0; c < OCEAN_CASCADES; c++) {
		float kLo = (c == 0) ? 0.0f
		          : CASCADE_BAND_HARMONIC*2.0f*(float)M_PI/water->patchM[c];
		float kHi = (c == OCEAN_CASCADES - 1) ? 1e30f
		          : CASCADE_BAND_HARMONIC*2.0f*(float)M_PI/water->patchM[c + 1];
		initH0(dim, water->patchM[c], kLo, kHi, h0k, conjH0mk);
		for(int i = 0; i < dim*dim; i++) {
			h0[4*i]   = h0k[i].real;
			h0[4*i+1] = h0k[i].im;
			h0[4*i+2] = conjH0mk[i].real;
			h0[4*i+3] = conjH0mk[i].im;
		}
		glGenTextures(1, &s_h0Tex[c]);
		glBindTexture(GL_TEXTURE_2D, s_h0Tex[c]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, dim, dim, 0, GL_RGBA, GL_FLOAT, h0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	free(h0);
	free(h0k);
	free(conjH0mk);

	for(int p = 0; p < 2; p++) {
		for(int t = 0; t < 2; t++)
			s_pingTex[p][t] = createPingTexture(dim);
		s_pingFBO[p] = createTargetFBO(s_pingTex[p], 2);
	}
	GLuint outs[4] = { water->dyTexture, water->dispTexture, water->slopeTexture, water->jacobTexture };
	for(int c = 0; c < OCEAN_CASCADES; c++)
		s_packFBO[c] = createLayerFBO(outs, 4, c);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Sampler units and uniform locations, once (the per-cascade values
	   are set in generateWavesGPU). */
	glUseProgram(s_specProg);
	glUniform1i(glGetUniformLocation(s_specProg, "h0"), 0);
	s_specPatch = glGetUniformLocation(s_specProg, "patchSize");
	s_specTime = glGetUniformLocation(s_specProg, "time");
	glUseProgram(s_fftProg);
	glUniform1i(glGetUniformLocation(s_fftProg, "srcA"), 0);
	glUniform1i(glGetUniformLocation(s_fftProg, "srcB"), 1);
	s_fftStage = glGetUniformLocation(s_fftProg, "stageSize");
	s_fftHorizontal = glGetUniformLocation(s_fftProg, "horizontal");
	glUseProgram(s_packProg);
	glUniform1i(glGetUniformLocation(s_packProg, "srcA"), 0);
	glUniform1i(glGetUniformLocation(s_packProg, "srcB"), 1);
	s_packScale = glGetUniformLocation(s_packProg, "scale");
	s_packDScale = glGetUniformLocation(s_packProg, "dscale");
	glUseProgram(0);
}

static void bindPair(int p) {
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_pingTex[p][0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, s_pingTex[p][1]);
}

static void generateWavesGPU(waves *water) {
	int dim = water->dimension;
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glViewport(0, 0, dim, dim);
	glBindVertexArray(s_emptyVAO);

	for(int c = 0; c < OCEAN_CASCADES; c++) {
		/* Spectrum at time t into pair 0. */
		glBindFramebuffer(GL_FRAMEBUFFER, s_pingFBO[0]);
		glUseProgram(s_specProg);
		/* Froude scaling: the shaders draw this cascade waveCascadeScale
		   times larger, so it runs sqrt of that slower and keeps the speed
		   of real waves of its drawn size. */
		glUniform1f(s_specTime, (float)(App_Time() / sqrt(waveCascadeScale(c))));
		glUniform1f(s_specPatch, water->patchM[c]);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, s_h0Tex[c]);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		/* Rows, then columns: 2*log2(dim) stages, an even count, so the
		   result lands back in pair 0. */
		glUseProgram(s_fftProg);
		int cur = 0;
		for(int axis = 0; axis < 2; axis++) {
			glUniform1i(s_fftHorizontal, axis == 0);
			for(int size = 2; size <= dim; size <<= 1) {
				glBindFramebuffer(GL_FRAMEBUFFER, s_pingFBO[1 - cur]);
				bindPair(cur);
				glUniform1i(s_fftStage, size);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				cur = 1 - cur;
			}
		}

		/* Pack into this cascade's layers. Heights and displacements share
		   one scale (sphere radii); derivatives are taken per patch length
		   (x the patch side), which water.vert/.frag turn into world slopes
		   with the cascade's tiling. */
		glBindFramebuffer(GL_FRAMEBUFFER, s_packFBO[c]);
		glUseProgram(s_packProg);
		glUniform1f(s_packScale, water->heightScale);
		glUniform1f(s_packDScale, water->heightScale*water->patchM[c]);
		bindPair(cur);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}

	/* Mips for every layer (see createWaveArray). Built with a framebuffer
	   bound that does not hold them, as TerrainGen does. */
	glBindFramebuffer(GL_FRAMEBUFFER, s_pingFBO[0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLuint mipped[4] = { water->slopeTexture, water->jacobTexture, water->dyTexture, water->dispTexture };
	for(int i = 0; i < 4; i++) {
		glBindTexture(GL_TEXTURE_2D_ARRAY, mipped[i]);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	}
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if(depth) glEnable(GL_DEPTH_TEST);
	if(blend) glEnable(GL_BLEND);
	if(cull) glEnable(GL_CULL_FACE);
}

void generateWaves(waves *water) {
	static int frame = 0;
	if(frame++ % WAVE_UPDATE_INTERVAL != 0)
		return;
	generateWavesGPU(water);
}
