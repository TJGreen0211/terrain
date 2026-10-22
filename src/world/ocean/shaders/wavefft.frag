#version 410

// One radix-2 Stockham stage of the unnormalized inverse FFT (same sign as
// water.c fft()), along rows or columns. Stockham reorders as it goes, so
// natural-order input comes out in natural order with no bit reversal.
// Each RGBA texel carries two complex values; both targets (four spectra)
// go through every stage together. Run log2(N) stages per axis with
// stageSize = 2, 4, ..., N, ping-ponging between two target pairs.
uniform sampler2D srcA;
uniform sampler2D srcB;
uniform int stageSize;      // subtransform length this stage produces
uniform int horizontal;     // 1 = along x (rows), 0 = along y (columns)

layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;

const float PI = 3.14159265359;

vec2 cmul(vec2 a, vec2 b)
{
	return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	int N = textureSize(srcA, 0).x;
	int index = horizontal == 1 ? p.x : p.y;
	int halfSize = stageSize / 2;
	int evenIndex = (index / stageSize) * halfSize + (index % halfSize);
	int oddIndex = evenIndex + N / 2;
	ivec2 pe = horizontal == 1 ? ivec2(evenIndex, p.y) : ivec2(p.x, evenIndex);
	ivec2 po = horizontal == 1 ? ivec2(oddIndex, p.y) : ivec2(p.x, oddIndex);

	// Twiddle exp(+2 pi i j / stageSize), j = index mod stageSize.
	float arg = 2.0 * PI * float(index % stageSize) / float(stageSize);
	vec2 tw = vec2(cos(arg), sin(arg));

	vec4 ea = texelFetch(srcA, pe, 0), oa = texelFetch(srcA, po, 0);
	vec4 eb = texelFetch(srcB, pe, 0), ob = texelFetch(srcB, po, 0);
	outA = vec4(ea.xy + cmul(tw, oa.xy), ea.zw + cmul(tw, oa.zw));
	outB = vec4(eb.xy + cmul(tw, ob.xy), eb.zw + cmul(tw, ob.zw));
}
