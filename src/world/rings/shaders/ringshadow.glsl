// Ring shadow (rings.c) for lit surfaces (quad.frag, water.frag,
// clouds.frag); uniforms from Rings_BindReceiver. Analytic rather than
// shadow-mapped: the ring is semi-transparent and banded, which a depth map
// cannot store. The sun ray from p crosses the ring plane at t; the
// transmittance there is exp(-tau / |mu|) with tau from the baked radial
// profile, looked up blurred by the sun's angular size over t (a physically
// sized penumbra).
uniform float hasRing;
uniform sampler1D ringProfile;
uniform vec3 ringCenter;     // camera-relative planet centre
uniform vec3 ringNormal;
uniform vec2 ringRadii;      // inner, outer (world units)
uniform float ringSunAngle;

// p relative to the ring centre, l the unit direction to the sun, maxT the
// distance to the sun.
float ringTransmittance(vec3 p, vec3 l, float maxT) {
	float mu = dot(l, ringNormal);
	if (hasRing < 0.5 || abs(mu) < 1e-4)
		return 1.0;
	float t = -dot(p, ringNormal) / mu;
	if (t <= 0.0 || t >= maxT)
		return 1.0;
	float r01 = (length(p + l * t) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	if (r01 <= 0.0 || r01 >= 1.0)
		return 1.0;
	float texelW = (ringRadii.y - ringRadii.x) / float(textureSize(ringProfile, 0));
	float tau = textureLod(ringProfile, r01, log2(max(t * ringSunAngle / texelW, 1.0))).a;
	return exp(-tau / abs(mu));
}
