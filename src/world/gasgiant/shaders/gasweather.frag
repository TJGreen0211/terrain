#version 410

// A gas giant's weather map for the cloud volume (gas_interior.c,
// cloud_volume.h): the exterior's cloud field (gasCloudField) over a patch
// of the tangent plane under the camera, turned into cover per layer.
//
// - Top layer (bright ammonia): over the zones, the pale bands, broken up
//   by the cloud texture, plus the high clouds.
// - Middle layer (rust): everywhere (never below 0.5), thicker where the
//   bands churn and around storms.
// - Towering: how high the clouds build, with the turbulence and storms.
// - Colour: the field's albedo, so from inside the clouds wear the colours
//   seen from space.

uniform vec3 mapCenter;        // unit, body-fixed
uniform vec3 mapEast, mapNorth;
uniform float mapExtent;       // m
uniform float mapSize;         // texels a side
uniform float radius;          // m
uniform float lumZone, lumBelt;   // the palette's zone and belt luminance

layout(location = 0) out vec4 Weather;
layout(location = 1) out vec4 Colour;

#include "engine/gfx/tonemap.glsl"
#include "world/gasgiant/gasgiant.glsl"

void main()
{
	vec2 xy = (gl_FragCoord.xy / mapSize - 0.5) * mapExtent;
	vec3 nb = normalize(mapCenter * radius + mapEast * xy.x + mapNorth * xy.y);
	GasCloud c = gasCloudField(nb, mapExtent / mapSize / radius);

	float lum = dot(c.col, vec3(0.3, 0.59, 0.11));
	float zone = clamp((lum - lumBelt) / max(lumZone - lumBelt, 0.02), 0.0, 1.0);
	float top = clamp(smoothstep(0.25, 0.85, zone) * (0.6 + 1.2 * (c.h - 0.5)) + 0.8 * c.hi, 0.0, 1.0);
	float mid = clamp(0.8 + 1.5 * (c.h - 0.5) + 0.2 * c.turb + 0.4 * c.storm, 0.5, 1.0);
	float tower = clamp(0.3 + 0.7 * c.turb + 0.6 * c.storm + 0.6 * (c.h - 0.5), 0.0, 1.0);

	Weather = vec4(top, mid, tower, 1.0);
	Colour = vec4(clamp(c.col, 0.0, 1.0), 1.0);
}
