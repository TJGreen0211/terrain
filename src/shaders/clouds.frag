#version 410

// The cloud layer as one slab at its middle altitude (Clouds.c; step 1 of
// docs/volumetric-clouds.md), over the full-detail planet.
//
// - Ray-traced against the sphere at the layer's middle, in planet radii
//   with the stable form the stand-ins use (miss distance |o x d|, hit point
//   from the closest approach): the planet is drawn in full from ~1e4 radii
//   away, where the textbook quadratic cancels.
// - Coverage: the weather maps plus detail octaves below them, moved along
//   the wind with the same two flow phases as the maps (cloudnoise.glsl).
// - Light: a plane-parallel cloud's diffuse reflectance (tops) or
//   transmission (base, seen from below), the sun reddened, set and
//   ring-shadowed like the ground's, skylight as on the ground, then the
//   extinction toward the camera and the scene's tone map. Blended over the
//   scene in display space like the water.
// - The air in front: the cloud adds the dome's in-scatter between the
//   camera and itself, and leaves its alpha in the target's alpha channel;
//   the dome then adds its whole column times (1 - alpha) (Clouds_Draw,
//   drawAtmosphereDome). Together: cloud + in front + behind x (1 - alpha),
//   exact for any alpha, with no depth threshold.

const float PI = 3.14159265359;

out vec4 FragColor;

uniform mat4 projection;
uniform vec2 viewportSize;
uniform mat3 viewToWorld;
uniform vec3 camPlanet;       // camera - planet centre, world axes, planet radii
uniform float planetRadius;   // m
uniform vec3 sunDir;          // unit, planet -> sun, world axes
uniform float sunAngle;       // sun's angular radius (sunset cut-off)

// Detail below the weather map.
uniform samplerCube cloudWind;  // the planet's wind (ocean) map (cube map)
uniform float windMax;
uniform float detailLambda0;  // first detail octave's wavelength, m
uniform vec3 cloudColor;

#include "atmosurface.glsl"
#include "ringshadow.glsl"
#include "engine/gfx/tonemap.glsl"

// The dome's march for the air in front of the cloud.
uniform float E;     // solar brightness (Atmosphere.c)
uniform float G_M;   // Mie asymmetry
vec2 rayIntersection(vec3 p, vec3 dir, float radius)
{
	return shellIntersection(p, dir, radius);
}
float scatterRingT(vec3 p, vec3 l)
{
	return ringTransmittance(p, l, 1e30);
}
#include "atmoscatter.glsl"
#include "cloudnoise.glsl"
#include "cloudlayer.glsl"

// Detail below the map: up to SLAB_OCTAVES octaves (Clouds.c builds a
// program per count), both flow phases side by side. Octaves under the
// footprint fade to the mean, as in cloudFbm. The count is a constant at
// every call, so the loop unrolls and its fetches issue together. Texture
// fetches are what this pass is bound by on an integrated GPU, so main()
// only pays for the full set at cloud edges.
#ifndef SLAB_OCTAVES
#define SLAB_OCTAVES 0
#endif
#define CORE_OCTAVES (SLAB_OCTAVES < 3 ? SLAB_OCTAVES : 3)
vec2 detailFbm(vec3 p0, vec3 p1, float lambda0, float foot, int octaves)
{
	vec3 q0 = p0 / lambda0, q1 = p1 / lambda0;
	float lambda = lambda0, amp = 1.0, norm = 0.0;
	vec2 sum = vec2(0.0);
	for (int k = 0; k < octaves; k++) {
		float fade = 1.0 - smoothstep(1.0, 2.0, foot / lambda);
		vec2 n = vec2(cloudValueNoise(q0), cloudValueNoise(q1));
		sum += amp * mix(vec2(0.5), n, fade);
		norm += amp;
		q0 = CLOUD_OCTAVE * q0 + vec3(17.3, -5.1, 9.7);
		q1 = CLOUD_OCTAVE * q1 + vec3(17.3, -5.1, 9.7);
		lambda *= 0.5;
		amp *= 0.5;
	}
	return norm > 0.0 ? sum / norm : vec2(0.5);
}

float depthOf(vec3 viewPos)
{
	vec4 c = vec4(viewPos, 1.0) * projection;
	return c.z / c.w;
}

void main()
{
	// View ray through this pixel (symmetric perspective), to world axes.
	vec2 ndc = gl_FragCoord.xy / viewportSize * 2.0 - 1.0;
	vec3 dView = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec3 d = viewToWorld * dView;

	// The layer's middle sphere, radius Rc (planet radii).
	vec3 o = camPlanet;
	float Rc = cloudRadius / planetRadius;
	vec3 oc = cross(o, d);
	float h2 = dot(oc, oc);
	if (h2 >= Rc * Rc)
		discard;
	float front = -dot(o, d);
	float halfChord = sqrt(Rc * Rc - h2);
	// From above the layer the near side (tops); from below, the far one
	// (the base overhead). cross(d, o x d) is o's part across the ray: the
	// closest approach to the centre, exact at any distance.
	bool above = dot(o, o) > Rc * Rc;
	float t;
	vec3 p;
	if (above) {
		if (front <= 0.0)
			discard;
		t = front - halfChord;
		p = cross(d, oc) - halfChord * d;
	} else {
		t = front + halfChord;
		p = cross(d, oc) + halfChord * d;
	}
	if (t <= 0.0)
		discard;

	vec3 up = normalize(p);
	float mu = dot(up, d);
	float pathF = cloudPathFactor(mu);
	// Pixel footprint at the hit (m), stretched by the slant.
	float footM = t * planetRadius * cloudPixAngle * min(pathF, 8.0);
	vec3 dBody = cloudWorldToBody * up;
	vec2 w = cloudWeather(dBody, footM / cloudRadius);

	// The detail moves the field by at most half the detail strength, and
	// the edge (or the anti-aliasing) blurs it a little further: beyond that
	// band it cannot change the coverage. Clear sky is done here; solid
	// cores only need the coarse octaves (for their texture); only the band
	// at the edges takes them all.
	float s = w.x - 0.5;
	float reach = 0.5 * cloudDetail + cloudEdge + 2.0 * fwidth(s);
	if (s < -reach)
		discard;

	// Detail octaves below the map, from ~4 map texels down to the pixel,
	// carried by the wind like the maps' noise.
	vec3 pBody = dBody * cloudRadius;
	vec3 wind = cloudWindAt(dBody, textureLod(cloudWind, dBody,
		log2(float(textureSize(cloudWind, 0).x)) - 5.0), windMax);
	vec3 p0 = pBody - wind * cloudFlowTau.x + cloudFlowSeed0 * detailLambda0;
	vec3 p1 = pBody - wind * cloudFlowTau.y + cloudFlowSeed1 * detailLambda0;
	vec2 nn = s > reach ? detailFbm(p0, p1, detailLambda0, footM, CORE_OCTAVES)
	                    : detailFbm(p0, p1, detailLambda0, footM, SLAB_OCTAVES);
	float n = clamp(0.5 + (cloudFlowBlend(nn.x, nn.y) - 0.5) * 2.0, 0.0, 1.0);
	float field = w.x - 0.5 + cloudDetail * (n - 0.5);
	float cov = cloudCover(w.x, n, 0.75 * fwidth(field));

	// Thickness varies with the detail too, so thick cores (where the
	// reflectance saturates) still show structure, and thins toward the
	// edges, which fray into translucent wisps.
	float tau = cloudOpacity * w.y * (0.35 + 1.3 * n) * mix(0.15, 1.0, smoothstep(0.0, 0.12, field));
	float opac = 1.0 - exp(-tau * pathF);
	float alpha = cov * opac;
	if (alpha < 0.002)
		discard;

	// Sunlight at the cloud: reddened and set by the air, cut by the rings.
	vec3 pm = p * planetRadius;              // planet-local, m
	float sunUp = dot(up, sunDir);
	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;   // quad.frag's
	vec3 sunCol = sunTransmittance(pm, sunDir, sunAngle) * ringTransmittance(pm, sunDir, 1e30);
	// Tops reflect, the base lets light through (two-stream slab).
	float Td = cloudDiffuseT(tau);
	float slab = above ? 1.0 - Td : Td;
	vec3 ambientSky = vec3(0.10, 0.12, 0.16);           // quad.frag's
	// Light leaving the cloudy part of the pixel, as quad.frag lights a
	// Lambert surface. Skylight scatters back in proportion to what the
	// cloud intercepts; the base sees less sky (the ground is below it).
	vec3 lit = cloudColor * (slab / PI * max(sunUp, 0.0) * sunRadiance * sunCol
	         + (ambientSky * skyFraction(sunUp) * (above ? 1.0 : 0.6) + NIGHT_FLOOR) * opac);
	// Straight colour: what the covered area reflects, per unit of opacity.
	vec3 col = lit * cov / alpha;
	vec3 eye = o * planetRadius;
	if (hasAtmosphere > 0.5)
		col = applyAtmosphere(col, eye, pm);
	vec3 display = toneMap(col);
	if (hasAtmosphere > 0.5) {
		// The dome's in-scatter from the camera (or where it enters the
		// air) to the cloud, in display units as the dome adds it.
		vec2 e = rayIntersection(eye, d, fOuterRadius);
		e.x = max(e.x, 0.0);
		e.y = min(e.y, t * planetRadius);
		if (e.y > e.x)
			display += inScatter(eye, d, e, sunDir, 1.0 / fScaleHeight, 1.0 / (fOuterRadius - fInnerRadius));
	}

	FragColor = vec4(display, alpha);
	// Depth-tested against the scene (terrain standing above the layer).
	gl_FragDepth = depthOf(dView * (t * planetRadius));
}
