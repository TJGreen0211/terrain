#version 410

// Starting spectrum h0(k) of one ocean tile (Water.c buildSpectrum), in the
// layout wavespectrum.frag reads: h0(k) and conj(h0(-k)). Texel x = M (wave
// number along the plane's E axis), y = N (along N), natural FFT order.
//
// Wave energy: JONSWAP frequency spectrum for wind speed U and fetch F
// (Hasselmann et al. 1973), capped at a fully developed sea (Pierson-
// Moskowitz), spread over direction with Donelan-Banner (1985), converted to
// a wave-number spectrum S(k) with deep-water dispersion w = sqrt(g k).
//
// Tiles share wavelengths so none repeats visibly: each tile's window spans
// two bands, u = log(|k| / band.x) / log(ratio) in [0, 2], with amplitude
// weight sin(pi u / 2). A band is the upper half of one tile and the lower
// half of the next, whose weights there are cos and sin of the same angle,
// so their energies (weight^2) sum to exactly 1: every wavelength comes from
// two tiles of incommensurate size, and nothing is counted twice. The top of
// a window is 32 cycles across the tile, half its Nyquist, so the mips can
// average it without moire. edges.x: the first tile also keeps everything
// below its window; edges.y: the last keeps its upper band whole (nobody
// shares those bands).
//
// Amplitudes are absolute: with complex Gaussian noise xi (unit-variance
// parts), h0 = xi * sqrt(S) * dk / 2 makes E|h0|^2 = S dk^2 / 2, so the
// realized height field (h0 e^{iwt} + conj(h0(-k)) e^{-iwt}, summed by an
// unnormalized inverse FFT) has variance sum S dk^2 = the spectrum's m0:
// heights come out in metres with no calibration.
uniform int dim;
uniform int layer;
uniform uint seed;
uniform float tileSize;     // m
uniform vec2 band;          // window base |k| (rad/m), log(size ratio to the next tile)
uniform vec2 edges;         // (first tile, last tile) flags
uniform float windSpeed;    // m/s
uniform vec2 windDir;       // unit, in the wave plane's (E, N) axes
uniform float fetch;        // m

out vec4 outH0;

const float PI = 3.14159265359;
const float G = 9.81;
// Dimensionless fetch g F / U^2 at which the sea is fully developed: where
// JONSWAP's peak frequency reaches Pierson-Moskowitz's 0.877 g/U, i.e.
// (22 / 0.877)^3. A developed sea also loses the JONSWAP peak enhancement
// (gamma 3.3 -> 1), so the cap lands on the Pierson-Moskowitz spectrum
// instead of overshooting it ~2.4x in energy.
const float FULLY_DEVELOPED = 15790.0;
const float GAMMA = 3.3;
// Viscous damping length (m): trims the shortest capillary ripples.
const float CAPILLARY_L = 0.002;

uint hashU(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// Complex Gaussian (unit-variance real and imaginary parts) per wave-number
// bin and tile, from a hash: the same bin always gets the same noise, so a
// rebuilt spectrum (new wind) keeps the waves' identity.
vec2 gauss(ivec2 id)
{
	uint h = hashU(uint(id.x) ^ hashU(uint(id.y) ^ hashU(uint(layer) ^ seed)));
	uint h2 = hashU(h ^ 0x9e3779b9u);
	float u1 = (float(h >> 8) + 1.0) / 16777217.0;    // (0, 1]
	float u2 = float(h2 >> 8) / 16777216.0;
	return sqrt(-2.0 * log(u1)) * vec2(cos(2.0 * PI * u2), sin(2.0 * PI * u2));
}

// Peak frequency and JONSWAP energy at frequency w (m^2 s).
float peakFrequency()
{
	float fd = min(G * fetch / (windSpeed * windSpeed), FULLY_DEVELOPED);
	return 22.0 * (G / windSpeed) * pow(fd, -1.0 / 3.0);
}

float jonswap(float w, float wp)
{
	float fd = min(G * fetch / (windSpeed * windSpeed), FULLY_DEVELOPED);
	float alpha = 0.076 * pow(fd, -0.22);
	float sigma = w <= wp ? 0.07 : 0.09;
	float d = (w - wp) / (sigma * wp);
	float r = exp(-0.5 * d * d);
	float q = wp / w;
	float gamma = mix(GAMMA, 1.0, smoothstep(0.3 * FULLY_DEVELOPED, FULLY_DEVELOPED, fd));
	return alpha * G * G / pow(w, 5.0) * exp(-1.25 * q * q * q * q) * pow(gamma, r);
}

// Donelan-Banner directional spreading, normalized over theta in (-pi, pi].
float donelanBanner(float theta, float w, float wp)
{
	float x = w / wp;
	float beta;
	if (x < 0.95)
		beta = 2.61 * pow(x, 1.3);
	else if (x < 1.6)
		beta = 2.28 * pow(x, -1.3);
	else
		beta = pow(10.0, -0.4 + 0.8393 * exp(-0.567 * log(x * x)));
	float c = cosh(beta * theta);
	return beta / (2.0 * tanh(beta * PI)) / (c * c);
}

// This tile's share (amplitude weight) of wave number |k|.
float bandWeight(float kl)
{
	float u = log(kl / band.x) / band.y;
	if (u >= 2.0)
		return 0.0;
	if (u < 1.0 && edges.x > 0.5)
		return 1.0;
	if (u < 0.0)
		return 0.0;
	if (u >= 1.0 && edges.y > 0.5)
		return 1.0;
	return sin(0.5 * PI * u);
}

// Expected |h0| at wave vector k for this tile (0 outside its window).
float amplitude(vec2 k, float wp)
{
	float kl = length(k);
	if (kl < 1e-6)
		return 0.0;
	float wgt = bandWeight(kl);
	if (wgt <= 0.0)
		return 0.0;
	float w = sqrt(G * kl);
	float dwdk = G / (2.0 * w);
	float theta = atan(k.x * windDir.y - k.y * windDir.x, dot(k, windDir));
	// S(kx, kz) = S(w) D(theta) dw/dk / k (polar -> Cartesian wave numbers).
	float S = jonswap(w, wp) * donelanBanner(theta, w, wp) * dwdk / kl;
	S *= exp(-kl * kl * CAPILLARY_L * CAPILLARY_L);
	float dk = 2.0 * PI / tileSize;
	return wgt * sqrt(S) * dk * 0.5;
}

vec2 waveVector(ivec2 id)
{
	vec2 idx = vec2(id);
	idx -= float(dim) * step(float(dim) * 0.5, idx);
	return 2.0 * PI * idx / tileSize;
}

void main()
{
	ivec2 id = ivec2(gl_FragCoord.xy);
	// Nyquist bins are their own mirror: i*k*h there is not Hermitian and
	// would leak into the packed imaginary channels.
	if (id.x == dim / 2 || id.y == dim / 2) {
		outH0 = vec4(0.0);
		return;
	}
	float wp = peakFrequency();
	ivec2 mirror = (ivec2(dim) - id) % dim;
	vec2 h0k = gauss(id) * amplitude(waveVector(id), wp);
	vec2 h0mk = gauss(mirror) * amplitude(waveVector(mirror), wp);
	outH0 = vec4(h0k, h0mk.x, -h0mk.y);
}
