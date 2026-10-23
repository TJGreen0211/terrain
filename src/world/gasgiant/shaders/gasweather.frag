#version 410

// A gas giant's weather map for the cloud volume (gas_interior.c,
// cloud_volume.h): the exterior's cloud field (gasCloudField) over a patch
// of the tangent plane under the camera, turned into cover per layer.
//
// The exterior field hardly changes across a map this size (a few degrees
// of latitude), so the map adds its own mesoscale structure: ranges of
// cloud about massifScale across (swept east-west like the bands, so
// several times longer) with wide canyons of clear air between them, and
// a wind shear that wanders across them.
//
// - Top layer (bright ammonia): over the zones, the pale bands, broken up
//   by the cloud texture, plus the high clouds, plus sparse bright
//   patches everywhere.
// - Middle layer (rust): the ranges, thicker where the bands churn and
//   around storms; thin in the canyons.
// - Towering: how high the clouds build: the ranges' cores, the
//   turbulence and the storms; full height right from a range's edge,
//   so its sides are cliffs.
// - Shear (a): signed, 0.5 = none. The bands' jets run along their edges,
//   so the wind shears most where the band colour changes fastest with
//   latitude; the sign is which way it does. It follows the storm-spun
//   latitude, so it wraps around the storms.
// - Colour: the field's albedo, so from inside the clouds wear the colours
//   seen from space. Its alpha is the storm walls: the collars around the
//   spinning storms and polar cyclones.

uniform vec3 mapCenter;        // unit, body-fixed
uniform vec3 mapEast, mapNorth;
uniform float mapExtent;       // m
uniform float mapSize;         // texels a side
uniform float radius;          // m
uniform float lumZone, lumBelt;   // the palette's zone and belt luminance
uniform float massifScale;     // m: the cloud ranges' size north-south

layout(location = 0) out vec4 Weather;
layout(location = 1) out vec4 Colour;

#include "engine/gfx/tonemap.glsl"
#include "world/gasgiant/gasgiant.glsl"

void main()
{
	vec2 xy = (gl_FragCoord.xy / mapSize - 0.5) * mapExtent;
	// Straight down onto the sphere, as the clouds read the map
	// (cloudshape.glsl: the point's offsets along east and north).
	vec3 nb = normalize(mapCenter * sqrt(max(radius * radius - dot(xy, xy), 0.0)) + mapEast * xy.x + mapNorth * xy.y);
	GasCloud c = gasCloudField(nb, mapExtent / mapSize / radius);

	float lum = dot(c.col, vec3(0.3, 0.59, 0.11));
	float zone = clamp((lum - lumBelt) / max(lumZone - lumBelt, 0.02), 0.0, 1.0);
	// The ranges: value noise, warped, in the storm-spun frame (so they
	// wind around the storms), stretched east-west as much as the bands.
	vec3 qm = vec3(c.ps.xy / c.st, c.ps.z) * (radius / massifScale) + gasSeedOffset * 3.1;
	float fpm = mapExtent / mapSize / massifScale;
	vec4 mw = gasFbm4(qm * 0.5 + vec3(3.7, 1.3, 8.1), 3, fpm * 0.5);
	vec4 m = gasFbm4(qm + vec3(mw.xy - 0.5, 0.0) * 1.6, 4, fpm);
	float ridge = m.x + 0.1 * (c.h - 0.5) + 0.06 * c.turb;
	float range = smoothstep(0.45, 0.6, ridge);

	float top = clamp(smoothstep(0.25, 0.85, zone) * (0.6 + 1.2 * (c.h - 0.5)) + 0.8 * c.hi, 0.0, 1.0);
	top = max(top, 0.55 * smoothstep(0.56, 0.68, m.z));
	float mid = clamp(mix(0.12, 1.0, range) + 0.4 * c.storm, 0.0, 1.0);
	// Rising from a range's edge toward its middle: its sides slope.
	float tower = clamp(mix(0.25, 0.85, smoothstep(0.47, 0.62, ridge)) + 0.15 * smoothstep(0.6, 0.72, ridge)
	                    + 0.3 * c.turb + 0.6 * c.storm, 0.0, 1.0);

	// Band luminance either side, over about a third of a degree.
	float lat = asin(clamp(c.ps.z, -1.0, 1.0));
	const float e = 0.006;
	vec3 lo = textureLod(gasProfile, (lat - e) / GAS_PI + 0.5, 0.0).rgb;
	vec3 hi = textureLod(gasProfile, (lat + e) / GAS_PI + 0.5, 0.0).rgb;
	float grad = dot(hi - lo, vec3(0.3, 0.59, 0.11)) / max(lumZone - lumBelt, 0.02);
	float shear = clamp(grad * 2.5 + (m.y - 0.5) * 6.0, -1.0, 1.0);
	float wall = clamp(c.wall * 2.5, 0.0, 1.0);

	Weather = vec4(top, mid, max(tower, wall), 0.5 + 0.5 * shear);
	Colour = vec4(clamp(c.col, 0.0, 1.0), wall);
}
