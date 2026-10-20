#version 410

uniform vec3 camPosition;
uniform vec3 C_R;
uniform float E;
uniform vec4 lightPos;
uniform vec3 shellCenter;   // camera-relative body centre (shell sphere origin)

uniform float fInnerRadius;
uniform float fOuterRadius;

in vec3 fLocalPos;

out vec4 FragColor;

const float PI = 3.14159265359;
// Intersection miss sentinel must exceed the whole scaled scene. The
// atmosphere shells can be millions of world units across.
const float MAX = 1e30;

const float K_R = 0.166;//Rayleigh constant > thicker
const float K_M = 0.0025;//Mie constant > thicker
const float G_M = -0.85;

// SCALE_H / SCALE_L are computed per invocation in main() and passed down:
// non-constant global initializers reading uniforms are not valid core GLSL
// at #version 410 (they were only legalised in 4.20 / 420pack).

const int numOutScatter = 5;
const float fNumOutScatter = 5.0;
const int numInScatter = 5;
const float fNumInScatter = 5.0;

vec3 rayDirection(vec3 camPosition) {
	// fLocalPos (dome vertex minus planet centre) and camPosition (camera
	// minus planet centre) are both planet-local, matching the origin-centred
	// rayIntersection spheres. Identical to the old m*fPosition form for the
	// T*S dome matrix, but robust if the model ever gains rotation.
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
float density(vec3 p, float scaleH) {
	return exp(-(length(p) - fInnerRadius) * scaleH);
}

float optic(vec3 p, vec3 q, float scaleH, float scaleL) {
	vec3 step = (q - p) / fNumOutScatter;
	vec3 v = p + step * 0.5;

	float sum = 0.0;
	for(int i = 0; i < numOutScatter; i++) {
		sum += density(v, scaleH);
		v += step;
	}
	sum *= length(step)*scaleL;
	return sum;
}

vec3 inScatter(vec3 o, vec3 dir, vec2 e, vec3 l, float scaleH, float scaleL) {
	float len = (e.y - e.x) / fNumInScatter;
	vec3 step = dir * len;
	vec3 p = o + dir * e.x;
	vec3 v = p + dir * (len * 0.5);

	// Camera-to-sample optical depth is accumulated along the view ray from
	// the in-scatter samples themselves (midpoint rule: everything before
	// this sample plus half its own step) instead of re-marching p -> v with
	// numOutScatter sub-samples at every step. Only the sample-to-sun leg
	// still needs its own march.
	vec3 sum = vec3(0.0);
	float viewOptic = 0.0;
	float segL = len * scaleL;
	for(int i = 0; i < numInScatter; i++) {
		vec2 f = rayIntersection(v, l, fOuterRadius);
		vec3 u = v + l * f.y;
		float dv = density(v, scaleH);
		float n = (viewOptic + 0.5 * dv * segL + optic(v, u, scaleH, scaleL))*(PI * 4.0);
		sum += dv * exp(-n * ( K_R * C_R + K_M ));
		viewOptic += dv * segL;
		v += step;
	}
	sum *= segL;
	float c = dot(dir, -l);
	float cc = c * c;
	return sum * ( K_R * C_R * rayleighPhase( cc ) + K_M * miePhase( G_M, c, cc ) ) * E;
}

void main (void)
{
	vec3 dir = rayDirection(camPosition);
	vec3 eye = camPosition;

	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	vec2 e = rayIntersection(eye, dir, fOuterRadius);
	if ( e.x > e.y ) {
		discard;
	}
	vec2 f = rayIntersection(eye, dir, fInnerRadius);
	// camera inside the atmosphere: the ray starts at the camera (A = camera)
	e.x = max(e.x, 0.0);
	// Only stop at the planet when its near-side intersection is ahead of the
	// camera. For rays pointing away from the planet, f.x is behind the eye;
	// clipping to it would discard the outward-facing part of the dome.
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if ( e.y <= e.x ) {
		discard;
	}

	// direction from the scattering path towards the light, evaluated at the
	// path midpoint rather than at the dome fragment: the dome vertex sits on
	// the outer shell, so using its world position parallaxes the sun
	// direction by up to the shell thickness against the surface shading that
	// quad.frag computes at the surface. Same light position both passes
	// use (rotateLight() in Main.c) so the terminators agree.
	vec3 l = (lightPos.w != 0.0)
		? normalize(lightPos.xyz - shellCenter - (eye + dir * (e.x + e.y) * 0.5))
		: normalize(lightPos.xyz);

	vec3 I = inScatter(eye, dir, e, l, scaleH, scaleL);

	FragColor = vec4(I, 1.0);
}
