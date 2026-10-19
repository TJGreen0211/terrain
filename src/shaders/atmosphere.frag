#version 410

uniform float E;
uniform vec3 camPosition;
uniform vec3 C_R;


in vec3 fLocalPos;

out vec4 FragColor;

const float PI = 3.14159265359;
const float degToRad = PI / 180.0;
const float MAX = 1e30;

const float DEG_TO_RAD = PI / 180.0;
float K_R = 0.166;//Rayleigh constant > thicker
float K_M = 0.0025;//Mie constant > thicker
//const float E = 14.3;//Sun brightness
//const vec3 C_R = vec3(0.3, 0.7, 1.0);
const float G_M = -0.85;

uniform float fInnerRadius;
uniform float fOuterRadius;
uniform vec3 lightPosition;
uniform vec3 shellCenter;

// Per-fragment scale factors. Computed from the uniforms at each use instead of
// as global initializers: initialising a global from a uniform is undefined and
// several drivers evaluate it once (while the uniforms are still 0), which made
// the whole shell scatter to zero.
float scaleH() { return 4.0 / (fOuterRadius - fInnerRadius); }
float scaleL() { return 1.0 / (fOuterRadius - fInnerRadius); }

const int numOutScatter = 3;
const float fNumOutScatter = 3.0;
const int numInScatter = 5;
const float fNumInScatter = 5.0;

vec3 rayDirection(vec3 camPosition) {
	// Both positions are planet-local, matching the origin-centred spheres
	// used by rayIntersection.
	return normalize(fLocalPos - camPosition);
}

vec2 rayIntersection(vec3 p, vec3 dir, float radius ) {
	float b = dot( p, dir );
	float c = dot( p, p ) - radius * radius;

	float d = b * b - c;
	if ( d < 0.0 ) {
		return vec2( MAX, -MAX );
	}
	d = sqrt( d );

	float near = -b - d;
	float far = -b + d;

	return vec2(near, far);
}

// Mie
// g : ( -0.75, -0.999 )
//      3 * ( 1 - g^2 )               1 + c^2
// F = ----------------- * -------------------------------
//      2 * ( 2 + g^2 )     ( 1 + g^2 - 2 * g * c )^(3/2)
float miePhase( float g, float c, float cc ) {
	float gg = g * g;

	float a = ( 1.0 - gg ) * ( 1.0 + cc );

	float b = 1.0 + gg - 2.0 * g * c;
	b *= sqrt( b );
	b *= 2.0 + gg;

	return 1.5 * a / b;
}

// Reyleigh
// g : 0
// F = 3/4 * ( 1 + c^2 )
float rayleighPhase( float cc ) {
	return 0.75 * ( 1.0 + cc );
}

//exp(fScaleOverScaleDepth * (fInnerRadius - fHeight));
float density(vec3 p) {
	return exp(-(length(p) - fInnerRadius) * scaleH());
}

float optic(vec3 p, vec3 q) {
	vec3 step = (q - p) / fNumOutScatter;
	vec3 v = p + step * 0.5;

	float sum = 0.0;
	for(int i = 0; i < numOutScatter; i++) {
		sum += density(v);
		v += step;
	}
	sum *= length(step)*scaleL();
	return sum;
}

vec3 inScatter(vec3 o, vec3 dir, vec2 e, vec3 l) {
	float len = (e.y - e.x) / fNumInScatter;
	vec3 step = dir * len;
	vec3 p = o + dir * e.x;
	vec3 v = p + dir * (len * 0.5);

	vec3 sum = vec3(0.0);
	for(int i = 0; i < numInScatter; i++) {
		vec2 f = rayIntersection(v, l, fOuterRadius);
		vec3 u = v + l * f.y;
		float n = (optic(p, v) + optic(v, u))*(PI * 4.0);
		sum += density(v)* exp(-n * ( K_R * C_R + K_M ));
		v += step;
	}
	sum *= len * scaleL();
	float c = dot(dir, -l);
	float cc = c * c;
	return sum * ( K_R * C_R * rayleighPhase( cc ) + K_M * miePhase( G_M, c, cc ) ) * E;
}

void main (void)
{
	vec3 dir = rayDirection(camPosition);
	vec3 eye = camPosition;

	vec2 e = rayIntersection(eye, dir, fOuterRadius);
	if ( e.x > e.y ) {
		discard;
	}
	vec2 f = rayIntersection(eye, dir, fInnerRadius);
	// camera inside the atmosphere: the ray starts at the camera (A = camera)
	e.x = max(e.x, 0.0);
	// Only clip at the planet when its near intersection is ahead of the eye.
	// Otherwise an outward-facing ray can be incorrectly discarded.
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if ( e.y <= e.x ) {
		discard;
	}

	// Evaluate the light direction at the middle of the scattering path,
	// rather than at the outer-shell vertex.
	vec3 pathMidpoint = eye + dir * ((e.x + e.y) * 0.5);
	vec3 l = normalize(lightPosition - shellCenter - pathMidpoint);
	vec3 I = inScatter(eye, dir, e, l);

	FragColor = vec4(I, 1.0);
}
