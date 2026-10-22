// The cloud layer as the slab (clouds.frag) and the shadow receivers
// (quad.frag, water.frag) see it: the weather maps and the layer's shape.
// Uniforms from Clouds.c. Needs PI defined first.
//
// Weather map texel: R = coverage field + 0.5 (signed: cloud where it is
// above 0 once the detail noise is added), G = column thickness (fraction of
// a full column's optical depth), in the climate map's equirect layout over
// the body-fixed frame.
uniform float cloudsOn;
uniform sampler2D cloudMapA;      // weather at the previous map time ...
uniform sampler2D cloudMapB;      // ... and the next
uniform float cloudMapBlend;      // 0 = A, 1 = B
uniform float cloudMapMaxLod;
uniform float cloudTexelAngle;    // map texel at the equator, radians
uniform mat3 cloudWorldToBody;    // world axes -> body-fixed (map) axes
uniform float cloudRadius;        // layer middle, m from the planet centre
uniform float cloudThickness;     // m
uniform float cloudOpacity;       // optical depth of a full column
uniform float cloudDetail;        // detail noise's pull on the field
uniform float cloudEdge;          // half-width of a cloud edge, field units
uniform float cloudPixAngle;      // radians per screen pixel (view centre)

// Diffuse transmission of a plane-parallel cloud of optical depth tau
// (two-stream, asymmetry g = 0.85): what gets through as scattered light.
// Its complement is the cloud's reflectance (no absorption).
float cloudDiffuseT(float tau)
{
	return 1.0 / (1.0 + 0.75 * (1.0 - 0.85) * tau);
}

// Cloud area fraction from the map's field s (texel R) and the detail noise
// n (0.5 = its mean, where it has faded below a pixel): the field pushed up
// or down by the detail, thresholded at 0. aa: the field's change across a
// pixel, so edges anti-alias instead of shimmering.
float cloudCover(float s, float n, float aa)
{
	float f = s - 0.5 + cloudDetail * (n - 0.5);
	float w = max(cloudEdge, aa);
	return smoothstep(-w, w, f);
}

// Weather (coverage, thickness) at body-fixed direction d, blurred to a
// footprint of pixAngle radians on the sphere. The mip is explicit: the
// implicit derivative jumps where atan wraps the longitude.
vec2 cloudWeather(vec3 d, float pixAngle)
{
	float z = clamp(d.z, -1.0, 1.0);
	float cosLat = max(sqrt(max(1.0 - z * z, 1e-4)), 0.02);
	float lod = clamp(log2(max(pixAngle / cloudTexelAngle, 1e-4) / cosLat), 0.0, cloudMapMaxLod);
	vec2 uv = vec2((atan(d.y, d.x) / PI + 1.0) * 0.5, asin(z) / PI + 0.5);
	return mix(textureLod(cloudMapA, uv, lod).rg, textureLod(cloudMapB, uv, lod).rg, cloudMapBlend);
}

// Path factor 1/mu through the slab for a ray at cosine mu to the vertical,
// capped by the longest chord through a shell this thick (grazing rays).
float cloudPathFactor(float mu)
{
	float maxChord = 2.0 * sqrt(2.0 * cloudRadius * cloudThickness);
	return 1.0 / max(abs(mu), cloudThickness / maxChord);
}

// Fraction of direct sunlight reaching planet-local p (world axes, m) along
// unit l through the layer above it, with the diffusely transmitted part let
// back in. Weather map only (no detail octaves): the layer's mean erosion.
// footM: the receiver's pixel footprint (m), to blur the lookup.
float cloudShadow(vec3 p, vec3 l, float footM)
{
	if (cloudsOn < 0.5)
		return 1.0;
	float r = length(p);
	if (r >= cloudRadius)
		return 1.0;
	// Sun ray to the layer middle, from inside the sphere (the far root);
	// (r - Rc)(r + Rc) avoids squaring two planet-sized radii.
	float b = dot(p, l);
	float c = (r - cloudRadius) * (r + cloudRadius);
	vec3 q = p + l * (-b + sqrt(max(b * b - c, 0.0)));
	vec3 up = normalize(q);
	vec2 w = cloudWeather(cloudWorldToBody * up, footM / cloudRadius);
	float cov = cloudCover(w.x, 0.5, 0.0);
	float tau = cloudOpacity * w.y;
	float direct = exp(-tau * cloudPathFactor(dot(up, l)));
	float through = direct + (1.0 - direct) * cloudDiffuseT(tau);
	return 1.0 - cov * (1.0 - through);
}
