#version 410

// GPU port of water.c calcH0Row: the time-t Tessendorf spectra, two complex
// fields per RGBA target, in the same layout as the CPU arrays (texel x = M
// = east wave number, y = N = north wave number, natural FFT order).
uniform sampler2D h0;        // h0(k).re, h0(k).im, conj(h0(-k)).re, conj(h0(-k)).im
uniform float time;
uniform float patchSize;     // water.c PATCH_SIZE (m)

layout(location = 0) out vec4 outHD;   // h + i*dDx/dz,  Dx + i*Dz
layout(location = 1) out vec4 outSJ;   // dh/dx + i*dh/dz,  dDx/dx + i*dDz/dz

const float PI = 3.14159265359;

vec2 cmul(vec2 a, vec2 b)
{
	return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void main()
{
	ivec2 id = ivec2(gl_FragCoord.xy);
	float N = float(textureSize(h0, 0).x);
	// water.c waveNumber: indices past N/2 are the negative frequencies.
	vec2 idx = vec2(id);
	idx -= N * step(N * 0.5, idx);
	vec2 k = 2.0 * PI * idx / patchSize;
	float kl = max(length(k), 0.0001);
	float invK = 1.0 / kl;
	float w = sqrt(9.81 * kl);

	// Phase wrapped before cos/sin: GPU trig loses accuracy on the large
	// arguments w * t reaches after a long run.
	float ph = mod(w * time, 2.0 * PI);
	vec2 e = vec2(cos(ph), sin(ph));
	vec4 h0v = texelFetch(h0, id, 0);
	vec2 h = cmul(h0v.xy, e) + cmul(h0v.zw, vec2(e.x, -e.y));
	vec2 ih = vec2(-h.y, h.x);
	float kx = k.x, kz = k.y;

	// Same packing as calcH0Row (a*sa + b*sb, each line two real spectra).
	vec2 hDxz  = h + ih * (-kx * kz * invK);
	vec2 disp  = ih * (kx * invK) + h * (-kz * invK);
	vec2 slope = ih * kx + h * (-kz);
	vec2 jacob = h * (-kx * kx * invK) + ih * (-kz * kz * invK);
	outHD = vec4(hDxz, disp);
	outSJ = vec4(slope, jacob);
}
