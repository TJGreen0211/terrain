// The O'Neill single-scattering march (GPU Gems 2 ch. 16; the OpenGLSolar
// port), shared by the atmosphere dome (atmosphere.frag) and the clouds,
// which add the air in front of them (clouds.frag). The includer declares
// fInnerRadius, fOuterRadius, C_R, K_R, K_M, G_M, E and PI, includes
// atmodensity.glsl, and defines
//   vec2 rayIntersection(vec3 p, vec3 dir, float radius)   (near, far)
//   float scatterRingT(vec3 p, vec3 l)                     ring shadow

const int numOutScatter = 5;
const float fNumOutScatter = 5.0;
const int numInScatter = 5;
const float fNumInScatter = 5.0;

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

// Density at p (atmodensity.glsl; scaleH kept for the call sites).
float density(vec3 p, float scaleH) {
	return atmoDensity(length(p) - fInnerRadius);
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
		sum += dv * exp(-n * ( K_R * C_R + K_M )) * scatterRingT(v, l);
		viewOptic += dv * segL;
		v += step;
	}
	sum *= segL;
	float c = dot(dir, -l);
	float cc = c * c;
	return sum * ( K_R * C_R * rayleighPhase( cc ) + K_M * miePhase( G_M, c, cc ) ) * E;
}
