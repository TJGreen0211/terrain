/*
	* Author: Tyler Green
	
	This module generates a 2d patch of waves for the size of the patch dimensions.
*/

#include "Water.h"
#include <stdio.h>
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
	float noise00 = (float)rand()/(float)(RAND_MAX);
	float noise01 = (float)rand()/(float)(RAND_MAX);
	float noise02 = (float)rand()/(float)(RAND_MAX);
	float noise03 = (float)rand()/(float)(RAND_MAX);

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
	if(k2 < 0.0001) k2 = 0.0001;
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

void initH0(waves *water) {
	float L = 1000.0;
	int offset = 0;
	for(float N = 0.0; N < water->dimension; N +=1.0) {
		for(float M = 0.0; M < water->dimension; M +=1.0) {
			vec2 k = {2.0*M_PI*N/L, 2.0*M_PI*M/L};

			vec4 grnd = gaussRand();
			float h0 = 1/sqrt(2.0) * sqrt(phillips(k));
			vec2 kNegative = {-k.x, -k.y};
			float h0Conj = 1/sqrt(2.0) * sqrt(phillips(kNegative));
			water->tildeh0k[offset].real = grnd.x*h0; water->tildeh0k[offset].im = grnd.y*h0;
			water->conjTildeh0MK[offset].real = grnd.z*h0Conj; water->conjTildeh0MK[offset].im = grnd.w*h0Conj;
			water->conjTildeh0MK[offset] = complexConj(water->conjTildeh0MK[offset]);
			offset++;
		}
	}
}

void calcH0(waves *water) {//(int dim, complexType *dx, complexType *dy, complexType *dz, complexType *tildeh0kTemp, complexType *conjTildeh0MKTemp) {
	float L = 1000.0;
	int offset = 0;
	for(float N = 0.0; N < water->dimension; N +=1.0) {
		for(float M = 0.0; M < water->dimension; M +=1.0) {
			vec2 k = {2.0*M_PI*N/L, 2.0*M_PI*M/L};

			float magnitude = lengthvec2(k);
			if(magnitude < 0.0001) magnitude = 0.0001;
			float w = sqrt(9.81* magnitude);

			float cosinus = cos(w+glfwGetTime());
			float sinus = sin(w+glfwGetTime());

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
		c2 = -c2;
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

void initializeWaves(waves *water, int dim) {	
	water->dimension = dim;
	
	water->dx = malloc(dim*dim*sizeof(complexType));
	water->dy = malloc(dim*dim*sizeof(complexType));
	water->dz = malloc(dim*dim*sizeof(complexType));
	water->tildeh0k = malloc(dim*dim*sizeof(complexType));
	water->conjTildeh0MK = malloc(dim*dim*sizeof(complexType));
	
	initH0(water);
}

void generateWaves(waves *water) {
	int dim2 = water->dimension*water->dimension;

	float *displacementdx = malloc(dim2*4*sizeof(float));
	float *displacementdy = malloc(dim2*4*sizeof(float));
	float *displacementdz = malloc(dim2*4*sizeof(float));

	calcH0(water);//dimension, dx, dy, dz, water.tildeh0k, water.conjTildeh0MK);
	fft2d(water->dimension, water->dy);
	fft2d(water->dimension, water->dx);
	fft2d(water->dimension, water->dz);
	int index = 0;
	for(int i = 0; i < dim2; i++) {
		float dxh = fabsf(water->dx[i].real/(dim2));
		float dyh = fabsf(water->dy[i].real/(dim2));
		float dzh = fabsf(water->dz[i].real/(dim2));
		//float h = dy[i].real/(dimension*dimension);

		displacementdx[index] = dxh;
		displacementdx[index+1] = dxh;
		displacementdx[index+2] = dxh;
		displacementdx[index+3] = 1.0;

		displacementdy[index] = dyh;
		displacementdy[index+1] = dyh;
		displacementdy[index+2] = dyh;
		displacementdy[index+3] = 1.0;

		displacementdz[index] = dzh;
		displacementdz[index+1] = dzh;
		displacementdz[index+2] = dzh;
		displacementdz[index+3] = 1.0;

		index+=4;
	}
	
	//water->dx = displacementdx;
	//water->dy = displacementdy;
	//water->dz = displacementdz;

	free(displacementdx);
	free(displacementdy);
	free(displacementdz);
}
