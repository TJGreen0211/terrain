#version 410

// Anvil plumes for the cloud volume (cloud_volume.c, after each weather
// bake): the storm cells' tops sheared off and carried downwind. Each texel
// looks back upwind (the wind at the cloud tops runs east or west by the
// sign of the map's shear) over the plume's length, and keeps the
// strongest cell it finds, weakened with the distance it has travelled.
// The reads widen downwind (coarser mips of the cells), so a plume
// spreads sideways as it goes, as anvils fan out.
// Out: r = strength, g = how far downwind (0 at the cell, 1 at the
// plume's length). cloudshape.glsl turns it into a flat sheet near the
// top of the layer.

uniform sampler2D weatherMap;  // a = shear, signed (0.5 none)
uniform sampler2D cellMap;     // r = storm cells, mipmapped
uniform float mapSize;         // texels a side
uniform float plumeLength;     // as a share of the map's width

out vec2 Plume;

void main()
{
	vec2 uv = gl_FragCoord.xy / mapSize;
	float shear = textureLod(weatherMap, uv, 0.0).a * 2.0 - 1.0;
	// Downwind along the map's east (+u) or west; weak shear: weak plumes.
	vec2 dir = vec2(shear >= 0.0 ? 1.0 : -1.0, 0.0);
	float windy = mix(0.4, 1.0, abs(shear));
	const int N = 16;
	float best = textureLod(cellMap, uv, 0.0).r, age = 0.0;
	for (int k = 1; k <= N; k++) {
		float t = float(k) / float(N);
		float lod = log2(1.0 + 12.0 * t);
		float c = textureLod(cellMap, uv - dir * plumeLength * t, lod).r;
		// The blurred reads are fainter: lift them back toward a cell's
		// strength, then fade with the distance travelled.
		float v = min(c * (1.0 + 2.0 * t), 1.0) * exp(-1.6 * t);
		if (v > best) {
			best = v;
			age = t;
		}
	}
	Plume = vec2(best * windy, age);
}
