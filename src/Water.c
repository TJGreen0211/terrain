/*
	* Author: Tyler Green
	
	This module generates a 2d patch of waves for the size of the patch dimensions.
*/

#include "Water.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

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

static float waveNumber(float n, int dim, float L) {
	float idx = (n < (dim / 2.0f)) ? n : n - dim;
	return 2.0f*M_PI*idx/L;
}

void initH0(waves *water) {
	float L = 1000.0;
	int dim = water->dimension;
	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			vec2 k = {waveNumber((float)N, dim, L), waveNumber((float)M, dim, L)};

			vec4 grnd = gaussRand();
			float h0 = 1/sqrt(2.0) * sqrt(phillips(k));
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

void calcH0(waves *water) {
	float L = 1000.0;
	float t = glfwGetTime();
	int dim = water->dimension;
	int offset = 0;
	for(int N = 0; N < dim; N++) {
		for(int M = 0; M < dim; M++) {
			vec2 k = {waveNumber((float)N, dim, L), waveNumber((float)M, dim, L)};

			float magnitude = lengthvec2(k);
			if(magnitude < 0.0001) magnitude = 0.0001;
			float w = sqrt(9.81* magnitude);

			float cosinus = cos(w*t);
			float sinus = sin(w*t);

			//float cosinus = cos(w);
			//float sinus = sin(w);

			complexType expIWT = {cosinus, sinus};
			complexType invExpIWT = {cosinus, -sinus};

			water->dy[offset] = complexAdd(complexMult(water->tildeh0k[offset], expIWT), complexMult(water->conjTildeh0MK[offset], invExpIWT));
			//printf("%d r: %f i: %f\n", offset, tildeh0k[offset].real, tildeh0k[offset].im);
			water->dx[offset].real = 0.0; water->dx[offset].im = -k.x/magnitude;
			water->dx[offset] = complexMult(water->dx[offset], water->dy[offset]);
			water->dz[offset].real = 0.0; water->dz[offset].im = -k.y/magnitude;
			water->dz[offset] = complexMult(water->dz[offset], water->dy[offset]);

			//printf("[%f][%f] P(k): %f, P(-k): %f\n", N, M, phillips(k), phillips(kNegative));
			//printf("gauss.x: %f, gauss.y %f\n", grnd.x, grnd.y);
			//printf("dx: %f %f, dy: %f %f, dz: %f %f\n", dx.real, dx.im, dy.real, dy.im, dz.real, dz.im);
			offset++;
		}
	}
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
	int log2n = log(dim)/log(2);

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

void fft2d(int dim, complexType *c) {
	complexType *row = malloc(dim*2*sizeof(float));
	int index = 0;
	int cpIndex = 0;
	for(int j = 0; j < dim; j++) {
		for(int i = 0; i < dim; i++) {
			row[i].real = c[index].real;
			row[i].im = c[index].im;
			//printf("in: %d, real: %f, im: %f\n", index, row[i].real, row[i].im);
			//printf("0in: %d, %f+%fj\n", i, row[i].real, row[i].im);
			index++;
		}
		fft(dim, row);
		for(int i = 0; i < dim; i++) {
			c[cpIndex].real = row[i].real;
			c[cpIndex].im = row[i].im;
			//printf("out: %d, real: %f, im: %f\n", cpIndex, c[cpIndex].real, c[cpIndex].im);
			//printf("0out: %d, %f+%fj\n", cpIndex, c[cpIndex].real, c[cpIndex].im);
			cpIndex++;
		}
	}

	index = 0;
	for(int i = 0; i < dim; i++) {
		for(int j = 0; j < dim; j++) {
			row[j].real = c[dim*j+i].real;
			row[j].im = c[dim*j+i].im;
			//printf("in: %d, real: %f, im: %f\n", dim*j+index, row[j].real, row[j].im);
			//printf("1in: %d, %f+%fj\n", j, row[j].real, row[j].im);
		}

		fft(dim, row);
		for(int j = 0; j < dim; j++) {
			c[dim*j+i].real = row[j].real;
			c[dim*j+i].im = row[j].im;
			//printf("in: %d, real: %f, im: %f\n", dim*j+i, row[j].real, row[j].im);
			//printf("1out: %d, %f+%fj\n", j, c[(i+1)*(j+1)-1].real, c[(i+1)*(j+1)-1].im);
		}
	}
	free(row);
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

static GLuint createDetailNormalTexture(int size) {
	float *h = malloc(size*size*sizeof(float));
	unsigned char *data = malloc(size*size*4);
	for(int y = 0; y < size; y++)
		for(int x = 0; x < size; x++)
			h[y*size+x] = detailFbm((float)x, (float)y, size);
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
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(h);
	free(data);
	return tex;
}

static GLuint createWaveTexture(int dim) {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, dim, dim, 0, GL_RED, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

void initializeWaves(waves *water, int dim) {
	water->dimension = dim;
	
	water->dx = malloc(dim*dim*sizeof(complexType));
	water->dy = malloc(dim*dim*sizeof(complexType));
	water->dz = malloc(dim*dim*sizeof(complexType));
	water->tildeh0k = malloc(dim*dim*sizeof(complexType));
	water->conjTildeh0MK = malloc(dim*dim*sizeof(complexType));

	water->dxData = malloc(dim*dim*sizeof(float));
	water->dyData = malloc(dim*dim*sizeof(float));
	water->dzData = malloc(dim*dim*sizeof(float));

	water->dxTexture = createWaveTexture(dim);
	water->dyTexture = createWaveTexture(dim);
	water->dzTexture = createWaveTexture(dim);
	water->detailTexture = createDetailNormalTexture(128);

	initH0(water);

	// Calibrate displacement once so wave height is a fixed fraction of the
	// unit sphere radius no matter the spectrum tuning (planet is drawn on the
	// unit sphere; sea level = radius 1).
	{
		int dim2 = dim*dim;
		calcH0(water);
		fft2d(dim, water->dy);
		float sum = 0.0;
		for(int i = 0; i < dim2; i++) {
			float h = water->dy[i].real;
			sum += h*h;
		}
		float rms = sqrtf(sum/(float)dim2);
		float targetRms = 0.0025f;
		// Absorbs the 1/dim^2 unnormalized-IFFT factor: data = raw * heightScale.
		water->heightScale = (rms > 0.0f) ? targetRms/rms : 1.0f;
	}
}

void generateWaves(waves *water) {
	int dim = water->dimension;
	int dim2 = dim*dim;

	calcH0(water);
	fft2d(dim, water->dy);
	fft2d(dim, water->dx);
	fft2d(dim, water->dz);

	float scale = water->heightScale;
	for(int i = 0; i < dim2; i++) {
		water->dxData[i] = water->dx[i].real*scale;
		water->dyData[i] = water->dy[i].real*scale;
		water->dzData[i] = water->dz[i].real*scale;
	}

	glBindTexture(GL_TEXTURE_2D, water->dxTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_FLOAT, water->dxData);
	glBindTexture(GL_TEXTURE_2D, water->dyTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_FLOAT, water->dyData);
	glBindTexture(GL_TEXTURE_2D, water->dzTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_FLOAT, water->dzData);
	glBindTexture(GL_TEXTURE_2D, 0);
}
