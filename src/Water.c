/*
	* Author: Tyler Green
	
	This module generates a 2d patch of waves for the size of the patch dimensions.
*/

#include "Water.h"
#include "Common.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void initWavesGPU(waves *water);

complexType complexMult(complexType c0, complexType c1) {
	complexType c;
	c.real = c0.real * c1.real - c0.im * c1.im;
	c.im = c0.real * c1.im + c0.im * c1.real;
	return c;
}

complexType complexAdd(complexType c0, complexType c1) {
	complexType c;
	c.real = c0.real + c1.real;
	c.im = c0.im + c1.im;
	return c;
}

complexType complexConj(complexType c) {
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

// Side length (m) of the simulated ocean patch.
#define PATCH_SIZE 1000.0f

static float waveNumber(float n, int dim, float L) {
	float idx = (n < (dim / 2.0f)) ? n : n - dim;
	return 2.0f*M_PI*idx/L;
}

// Spectra are stored row-major: column M is texture u (east), row N is
// texture v (north), so kx comes from M and kz from N.
static vec2 waveVector(int N, int M, int dim) {
	vec2 k = {waveNumber((float)M, dim, PATCH_SIZE), waveNumber((float)N, dim, PATCH_SIZE)};
	return k;
}

/* Per-bin constants of the spectrum (wave vector, 1/|k|, dispersion
   omega = sqrt(g|k|)), built once by initH0 instead of every frame. One ocean
   exists, so file scope is enough. */
static vec2 *s_waveK;
static float *s_invK;
static float *s_omega;

static void initWaveTables(int dim) {
	free(s_waveK); free(s_invK); free(s_omega);
	s_waveK = malloc(dim*dim*sizeof(vec2));
	s_invK = malloc(dim*dim*sizeof(float));
	s_omega = malloc(dim*dim*sizeof(float));
	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			int offset = N*dim + M;
			vec2 k = waveVector(N, M, dim);
			float magnitude = lengthvec2(k);
			if(magnitude < 0.0001) magnitude = 0.0001;
			s_waveK[offset] = k;
			s_invK[offset] = 1.0f/magnitude;
			s_omega[offset] = sqrtf(9.81f*magnitude);
		}
	}
}

void initH0(waves *water) {
	int dim = water->dimension;
	initWaveTables(dim);
	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			vec2 k = waveVector(N, M, dim);

			vec4 grnd = gaussRand();
			float h0 = 1/sqrt(2.0) * sqrt(phillips(k));
			// Nyquist bins are their own mirror: i*k*h there is not Hermitian
			// and would leak into the packed imaginary channels.
			if(N == dim/2 || M == dim/2) h0 = 0.0;
			int offset = N*dim + M;
			water->tildeh0k[offset].real = grnd.x*h0; water->tildeh0k[offset].im = grnd.y*h0;
		}
	}

	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			int offset = N*dim + M;
			int mirror = ((dim - N) % dim)*dim + ((dim - M) % dim);
			water->conjTildeh0MK[offset] = complexConj(water->tildeh0k[mirror]);
		}
	}
}

// a*sa + b*sb
static complexType complexMad(complexType a, float sa, complexType b, float sb) {
	complexType c = {a.real*sa + b.real*sb, a.im*sa + b.im*sb};
	return c;
}

/* One row N of the time-t spectra. Bins are independent, so rows can be
   split across cores. */
static void calcH0Row(waves *water, float t, int N) {
	int dim = water->dimension;
	for(int M = 0; M < dim; M++) {
		int offset = N*dim + M;
		vec2 k = s_waveK[offset];
		float w = s_omega[offset];

		float cosinus = cosf(w*t);
		float sinus = sinf(w*t);

		complexType expIWT = {cosinus, sinus};
		complexType invExpIWT = {cosinus, -sinus};

		complexType h = complexAdd(complexMult(water->tildeh0k[offset], expIWT), complexMult(water->conjTildeh0MK[offset], invExpIWT));
		complexType ih = {-h.im, h.real};
		float kx = k.x, kz = k.y, invK = s_invK[offset];

		// Choppy displacement Dx <-> i kx/|k| h (Tessendorf's -i k/|k| h with
		// lambda < 0, i.e. Gerstner's x - A sin: points move toward crests),
		// dh/dx <-> i kx h, dDx/dx <-> -kx^2/|k| h. Each line is a + i*b of
		// two such real spectra.
		water->hDxz[offset]  = complexMad(h, 1.0f, ih, -kx*kz*invK);
		water->disp[offset]  = complexMad(ih, kx*invK, h, -kz*invK);
		water->slope[offset] = complexMad(ih, kx, h, -kz);
		water->jacob[offset] = complexMad(h, -kx*kx*invK, ih, -kz*kz*invK);

		//printf("[%f][%f] P(k): %f, P(-k): %f\n", N, M, phillips(k), phillips(kNegative));
		//printf("gauss.x: %f, gauss.y %f\n", grnd.x, grnd.y);
		//printf("dx: %f %f, dy: %f %f, dz: %f %f\n", dx.real, dx.im, dy.real, dy.im, dz.real, dz.im);
	}
}

void calcH0(waves *water) {
	float t = glfwGetTime();
	int N;
	#pragma omp parallel for schedule(static)
	for(N = 0; N < water->dimension; N++)
		calcH0Row(water, t, N);
}

void complexBitReverse(complexType *c, int dim) {
	for(unsigned int i = 0, j = 0; i < dim; i++) {
		if(i < j) {
			complexType tmp = {c[i].real, c[i].im};
			c[i].real = c[j].real;
			c[i].im = c[j].im;
			c[j].real = tmp.real;
			c[j].im = tmp.im;
		}

		unsigned bit = ~i & (i+1);
		unsigned rev = (dim / 2) / bit;
		j ^= (dim-1) & ~(rev-1);
	}
}

// Unnormalized inverse FFT; caller divides by dim*dim for the 2d case.
void fft(int dim, complexType *c) {
	complexBitReverse(c, dim);
	int log2n = 0;
	while((1 << log2n) < dim) log2n++;

	float c1 = -1.0;
	float c2 = 0.0;
	long l2 = 1;
	long i1;
	for(int l = 0; l < log2n; l++) {
		long l1 = l2;
		l2 <<= 1;
		float u1 = 1.0;
		float u2 = 0.0;
		for(int j = 0; j < l1; j++) {
			for(int i = j; i < dim; i+=l2) {
				i1 = i + l1;
				complexType temp = {u1 * c[i1].real - u2 * c[i1].im, u1 * c[i1].im + u2 * c[i1].real};
				c[i1].real = c[i].real - temp.real;
            	c[i1].im = c[i].im - temp.im;
            	c[i].real += temp.real;
            	c[i].im += temp.im;
			}
			float z = u1 * c1 - u2 * c2;
			u2 = u1 * c2 + u2 * c1;
			u1 = z;
		}
		c2 = sqrt((1.0 - c1) / 2.0);
		c1 = sqrt((1.0 + c1) / 2.0);
	}
}

/* Rows in place, then columns through a scratch copy. Every 1D transform is
   independent, so both passes split across cores (/openmp); each thread
   owns its scratch column. */
void fft2d(int dim, complexType *c) {
	int j;
	#pragma omp parallel for schedule(static)
	for(j = 0; j < dim; j++)
		fft(dim, c + j*dim);

	#pragma omp parallel
	{
		complexType *col = malloc(dim*sizeof(complexType));
		int i;
		#pragma omp for schedule(static)
		for(i = 0; i < dim; i++) {
			for(int j = 0; j < dim; j++)
				col[j] = c[dim*j+i];
			fft(dim, col);
			for(int j = 0; j < dim; j++)
				c[dim*j+i] = col[j];
		}
		free(col);
	}
}

static float hash2(int x, int y) {
	unsigned n = x*374761393u + y*668265263u;
	n = (n ^ (n >> 13)) * 1274126177u;
	n = n ^ (n >> 16);
	return (float)(n & 0xFFFFFF) / 16777215.0f;
}

static float vnoise(float x, float y, int period) {
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float xf = x - (float)xi, yf = y - (float)yi;
	float u = xf*xf*(3.0f-2.0f*xf), v = yf*yf*(3.0f-2.0f*yf);
	int x0 = ((xi % period) + period) % period, y0 = ((yi % period) + period) % period;
	int x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
	float a = hash2(x0, y0), b = hash2(x1, y0), c = hash2(x0, y1), d = hash2(x1, y1);
	return a + (b-a)*u + (c-a)*v + (a-b-c+d)*u*v;
}

static float detailFbm(float x, float y, int size) {
	float v = 0.0f, amp = 0.5f;
	for(int o = 0; o < 4; o++) {
		int period = 8 << o;
		v += amp * vnoise(x/(float)size*(float)period, y/(float)size*(float)period, period);
		amp *= 0.5f;
	}
	return v;
}

/* outVar receives the mean squared slope of the decoded rg (see detailVar). */
static GLuint createDetailNormalTexture(int size, float *outVar) {
	float *h = malloc(size*size*sizeof(float));
	unsigned char *data = malloc(size*size*4);
	for(int y = 0; y < size; y++)
		for(int x = 0; x < size; x++)
			h[y*size+x] = detailFbm((float)x, (float)y, size);
	double sum = 0.0, sum2 = 0.0;
	for(int y = 0; y < size; y++) {
		for(int x = 0; x < size; x++) {
			float hl = h[y*size + (x-1+size)%size];
			float hr = h[y*size + (x+1)%size];
			float hdn = h[((y-1+size)%size)*size + x];
			float hup = h[((y+1)%size)*size + x];
			float nx = (hl-hr)*8.0f, ny = (hdn-hup)*8.0f;
			float inv = 1.0f/sqrtf(nx*nx+ny*ny+1.0f);
			unsigned char *px = &data[(y*size+x)*4];
			px[0] = (unsigned char)((nx*inv*0.5f+0.5f)*255.0f);
			px[1] = (unsigned char)((ny*inv*0.5f+0.5f)*255.0f);
			px[2] = (unsigned char)((inv*0.5f+0.5f)*255.0f);
			px[3] = 255;
			for(int c = 0; c < 2; c++) {
				float d = ((float)px[c]/255.0f - 0.5f)*2.0f;
				sum += d; sum2 += d*d;
			}
		}
	}
	{
		double n = 2.0*size*size, mean = sum/n;
		*outVar = (float)(sum2/n - mean*mean);
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	/* Mipmapped: tiled 40x over the sphere, so toward the horizon it is
	   heavily minified and would otherwise thrash the texture cache. */
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(h);
	free(data);
	return tex;
}

static GLuint createWaveTexture(int dim, GLint internalFormat, GLenum format) {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, dim, dim, 0, format, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

void initializeWaves(waves *water, int dim) {
	water->dimension = dim;
	
	water->hDxz = malloc(dim*dim*sizeof(complexType));
	water->disp = malloc(dim*dim*sizeof(complexType));
	water->slope = malloc(dim*dim*sizeof(complexType));
	water->jacob = malloc(dim*dim*sizeof(complexType));
	water->tildeh0k = malloc(dim*dim*sizeof(complexType));
	water->conjTildeh0MK = malloc(dim*dim*sizeof(complexType));

	water->dyData = malloc(dim*dim*sizeof(float));
	water->dispData = malloc(dim*dim*2*sizeof(float));
	water->slopeData = malloc(dim*dim*4*sizeof(float));
	water->jacobData = malloc(dim*dim*4*sizeof(float));

	water->dyTexture = createWaveTexture(dim, GL_R32F, GL_RED);
	water->dispTexture = createWaveTexture(dim, GL_RG32F, GL_RG);
	water->slopeTexture = createWaveTexture(dim, GL_RGBA32F, GL_RGBA);
	/* RGBA, not RGB: 3-channel float uploads and mip builds are a slow
	   driver path; alpha is unused padding. */
	water->jacobTexture = createWaveTexture(dim, GL_RGBA32F, GL_RGBA);
	/* Fragment-sampled maps get a mip chain (refilled every frame): at
	   grazing views the ocean minifies them hard, and sampling level 0 there
	   thrashes the texture cache and shimmers. For the slope squares the mip
	   chain is also what turns averaged-away waves into roughness. */
	GLuint mipped[2] = { water->slopeTexture, water->jacobTexture };
	for(int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, mipped[i]);
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	water->detailTexture = createDetailNormalTexture(128, &water->detailVar);

	initH0(water);

	// Calibrate displacement once so wave height is a fixed fraction of the
	// unit sphere radius no matter the spectrum tuning (planet is drawn on the
	// unit sphere; sea level = radius 1).
	{
		int dim2 = dim*dim;
		calcH0(water);
		fft2d(dim, water->hDxz);
		float sum = 0.0;
		for(int i = 0; i < dim2; i++) {
			float h = water->hDxz[i].real;
			sum += h*h;
		}
		float rms = sqrtf(sum/(float)dim2);
		float targetRms = 0.0025f;
		// Absorbs the 1/dim^2 unnormalized-IFFT factor: data = raw * heightScale.
		water->heightScale = (rms > 0.0f) ? targetRms/rms : 1.0f;
	}
#if WAVES_ON_GPU
	initWavesGPU(water);
#endif
}

/* ---------------- GPU path (WAVES_ON_GPU) ----------------
   The same update as generateWavesCPU as fragment-shader passes, written
   straight into the textures water.vert/.frag sample, so nothing crosses
   the bus per frame:
     wavespectrum.frag  h0 -> the four time-t spectra (two RGBA targets),
     wavefft.frag       log2(N) Stockham stages along x, then along y,
                        ping-ponging between two target pairs,
     wavepack.frag      scale + pack into dy/disp/slope/jacob textures.
   One ocean exists, so file scope is enough. */
static GLuint s_specProg, s_fftProg, s_packProg;
static GLuint s_emptyVAO;
static GLuint s_h0Tex;
static GLuint s_pingTex[2][2];   /* [pair][A = hDxz|disp, B = slope|jacob] */
static GLuint s_pingFBO[2];
static GLuint s_packFBO;
static GLint s_specTime, s_fftStage, s_fftHorizontal;

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

static GLuint createTargetFBO(const GLuint *tex, int count) {
	static const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
	                                GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	for(int i = 0; i < count; i++)
		glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, tex[i], 0);
	glDrawBuffers(count, bufs);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "[water] GPU wave FBO incomplete\n");
	return fbo;
}

static void initWavesGPU(waves *water) {
	int dim = water->dimension;
	createShader(&s_specProg, "shaders/wavefft.vert", "shaders/wavespectrum.frag");
	createShader(&s_fftProg, "shaders/wavefft.vert", "shaders/wavefft.frag");
	createShader(&s_packProg, "shaders/wavefft.vert", "shaders/wavepack.frag");
	glGenVertexArrays(1, &s_emptyVAO);

	/* h0(k) and conj(h0(-k)), fixed for the run (initH0). */
	float *h0 = malloc((size_t)dim*dim*4*sizeof(float));
	for(int i = 0; i < dim*dim; i++) {
		h0[4*i]   = water->tildeh0k[i].real;
		h0[4*i+1] = water->tildeh0k[i].im;
		h0[4*i+2] = water->conjTildeh0MK[i].real;
		h0[4*i+3] = water->conjTildeh0MK[i].im;
	}
	glGenTextures(1, &s_h0Tex);
	glBindTexture(GL_TEXTURE_2D, s_h0Tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, dim, dim, 0, GL_RGBA, GL_FLOAT, h0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	free(h0);

	for(int p = 0; p < 2; p++) {
		for(int t = 0; t < 2; t++)
			s_pingTex[p][t] = createPingTexture(dim);
		s_pingFBO[p] = createTargetFBO(s_pingTex[p], 2);
	}
	GLuint outs[4] = { water->dyTexture, water->dispTexture, water->slopeTexture, water->jacobTexture };
	s_packFBO = createTargetFBO(outs, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Constant uniforms + sampler units, once. */
	glUseProgram(s_specProg);
	glUniform1i(glGetUniformLocation(s_specProg, "h0"), 0);
	glUniform1f(glGetUniformLocation(s_specProg, "patchSize"), PATCH_SIZE);
	s_specTime = glGetUniformLocation(s_specProg, "time");
	glUseProgram(s_fftProg);
	glUniform1i(glGetUniformLocation(s_fftProg, "srcA"), 0);
	glUniform1i(glGetUniformLocation(s_fftProg, "srcB"), 1);
	s_fftStage = glGetUniformLocation(s_fftProg, "stageSize");
	s_fftHorizontal = glGetUniformLocation(s_fftProg, "horizontal");
	glUseProgram(s_packProg);
	glUniform1i(glGetUniformLocation(s_packProg, "srcA"), 0);
	glUniform1i(glGetUniformLocation(s_packProg, "srcB"), 1);
	glUniform1f(glGetUniformLocation(s_packProg, "scale"), water->heightScale);
	glUniform1f(glGetUniformLocation(s_packProg, "dscale"), water->heightScale*PATCH_SIZE);
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

	/* Spectrum at time t into pair 0. */
	glBindFramebuffer(GL_FRAMEBUFFER, s_pingFBO[0]);
	glUseProgram(s_specProg);
	glUniform1f(s_specTime, (float)glfwGetTime());
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_h0Tex);
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

	/* Pack into the sampled textures. */
	glBindFramebuffer(GL_FRAMEBUFFER, s_packFBO);
	glUseProgram(s_packProg);
	bindPair(cur);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	/* Mips for the two fragment-sampled maps (see initializeWaves). Built
	   with a framebuffer bound that does not hold them, as TerrainGen does. */
	glBindFramebuffer(GL_FRAMEBUFFER, s_pingFBO[0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, water->slopeTexture);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, water->jacobTexture);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if(depth) glEnable(GL_DEPTH_TEST);
	if(blend) glEnable(GL_BLEND);
	if(cull) glEnable(GL_CULL_FACE);
}

static void generateWavesCPU(waves *water);

void generateWaves(waves *water) {
	static int frame = 0;
	if(frame++ % WAVE_UPDATE_INTERVAL != 0)
		return;
#if WAVES_ON_GPU
	generateWavesGPU(water);
#else
	generateWavesCPU(water);
#endif
}

/* CPU reference path: spectrum, FFTs and packing on the cores (/openmp),
   then a texture upload + mip build. */
static void generateWavesCPU(waves *water) {
	int dim = water->dimension;
	int dim2 = dim*dim;
	float t = glfwGetTime();
	complexType *spectra[4] = { water->hDxz, water->disp, water->slope, water->jacob };

	// Heights and displacements share one scale (sphere radii). Derivatives
	// are taken per patch length (x PATCH_SIZE); water.vert/.frag divide by
	// the patch's world size, which depends on waveTiling and latitude.
	float scale = water->heightScale;
	float dscale = scale*PATCH_SIZE;

	/* The whole update in one parallel region (spectrum, the four
	   transforms' row and column passes, packing) instead of a fork/join
	   per stage: each omp for ends in a barrier, which is all the ordering
	   the stages need. Each thread owns its column scratch. */
	#pragma omp parallel
	{
		complexType *col = malloc(dim*sizeof(complexType));
		int r;

		#pragma omp for schedule(static)
		for(r = 0; r < dim; r++)
			calcH0Row(water, t, r);

		#pragma omp for schedule(static)
		for(r = 0; r < 4*dim; r++)
			fft(dim, spectra[r/dim] + (r%dim)*dim);

		#pragma omp for schedule(static)
		for(r = 0; r < 4*dim; r++) {
			complexType *c = spectra[r/dim];
			int i = r%dim;
			for(int j = 0; j < dim; j++)
				col[j] = c[dim*j+i];
			fft(dim, col);
			for(int j = 0; j < dim; j++)
				c[dim*j+i] = col[j];
		}

		#pragma omp for schedule(static)
		for(r = 0; r < dim2; r++) {
			water->dyData[r] = water->hDxz[r].real*scale;
			water->dispData[2*r]   = water->disp[r].real*scale;
			water->dispData[2*r+1] = water->disp[r].im*scale;
			float sx = water->slope[r].real*dscale, sz = water->slope[r].im*dscale;
			water->slopeData[4*r]   = sx;
			water->slopeData[4*r+1] = sz;
			water->slopeData[4*r+2] = sx*sx;
			water->slopeData[4*r+3] = sz*sz;
			water->jacobData[4*r]   = water->jacob[r].real*dscale;
			water->jacobData[4*r+1] = water->jacob[r].im*dscale;
			water->jacobData[4*r+2] = water->hDxz[r].im*dscale;
			water->jacobData[4*r+3] = 0.0f;
		}

		free(col);
	}

	glBindTexture(GL_TEXTURE_2D, water->dyTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_FLOAT, water->dyData);
	glBindTexture(GL_TEXTURE_2D, water->dispTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RG, GL_FLOAT, water->dispData);
	/* The two maps water.frag samples are mipmapped (see initializeWaves);
	   height/displacement are only read by water.vert at the base level. */
	glBindTexture(GL_TEXTURE_2D, water->slopeTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RGBA, GL_FLOAT, water->slopeData);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, water->jacobTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RGBA, GL_FLOAT, water->jacobData);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
}
