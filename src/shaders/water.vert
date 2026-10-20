#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
// Quadtree patch instance (locations 4-10) + patchDir/patchSphere.
#include "patch.glsl"

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform sampler2D waveHeight;
uniform sampler2D waveDisp;    // choppy horizontal displacement (east, north)
uniform float waveChoppy;
uniform float oceanOffset;
uniform float waveAmp;
uniform float waveTiling;
// Same height field / sea level / LOD inputs as quad.vert, so the shell knows
// the water depth under each vertex and calms the waves toward the coast.
uniform sampler2D heightMap;
uniform float seaLevel;
uniform float oceanDepth;
uniform vec3 lodCameraPos;
uniform float lodK;

// Sea state (TerrainGen.c bakeOcean): R wind speed / windMax, A fetch.
uniform sampler2D oceanMap;
uniform float windMax;
// Planet temperature response (g_planetStyle), for the sea-ice mask.
uniform float tempOffset;
uniform float snowTemp;

const float PI = 3.14159265359;
// Normalized depth over which waves ramp from flat (coast) to full height.
const float SHORE_CALM = 0.25;
// Wind speed (m/s) of the sea the FFT amplitude was tuned for. Wave height
// goes as U^2 in a developed sea, cut back where the fetch is short.
const float SEA_STATE_REF_WIND = 7.0;
// Must match water.frag: sea ice sets in this far below the land snowline.
const float SEA_ICE_OFFSET = -0.04;

out vec3 fSurfDir;
out vec3 fragPos;
out float fWave;

void main()
{
	// Match quad.vert: the same patch point on the unit sphere (body-local
	// direction p) and its camera-relative position (patch.glsl). The shell
	// displacement below is the small offset from that sphere point, in
	// body radii.
	vec2 puv = vPosition.xy;
	vec3 p = patchDir(puv);
	vec3 sphere = patchSphere(puv, p, model);

	vec3 disp;
	float wave;
	if(vPosition.w > 0.5) {
		// Boundary skirt: wall vertices skip the wave displacement and sit
		// just under sea level, hiding LOD patch-edge cracks under the shell.
		disp = p * ((oceanOffset - 1.0) - 0.001);
		wave = 0.0;
	}
	else {
		// Long/lat UV computed from the direction; the DFT wave texture is
		// periodic, so the u = 0/1 seam is continuous.
		vec2 uv = vec2((atan(p.y, p.x) / 3.1415926 + 1.0) * 0.5,
		               (asin(p.z) / 3.1415926 + 0.5));
		// Water depth under this vertex, fetched like quad.vert so the
		// shell and the seafloor agree on the coastline.
		float cell = patchCell();
		float lodCell = log2(max(cell*2048.0/PI, 1.0));
		float lodPix = log2(max(length(sphere - lodCameraPos)*lodK, 1e-4));
		float th = textureLod(heightMap, uv, clamp(max(lodCell, lodPix), 0.0, 11.0)).r;
		float depthT = clamp((seaLevel - th) / max(oceanDepth, 1e-4), 0.0, 1.0);
		// Waves die out toward the shore: at the coastline the shell sits
		// exactly at sea level, where the planet's land relief starts.
		float calm = smoothstep(0.0, SHORE_CALM, depthT);
		// Sea state from the baked wind and fetch: calm doldrums and lee
		// coasts, heavy seas under the westerlies.
		vec4 sea = textureLod(oceanMap, uv, clamp(max(lodCell, lodPix), 0.0, 11.0));
		float U = sea.r * windMax;
		calm *= clamp(pow(U / SEA_STATE_REF_WIND, 2.0) * mix(0.3, 1.0, sea.a), 0.15, 2.0);
		// Sea ice holds the surface flat (same temperature as quad.frag's
		// land snow, at sea level, without the noise).
		float seaTemp = sqrt(max(1.0 - p.z*p.z, 0.0)) * 1.2 - 0.2 + tempOffset;
		calm *= smoothstep(snowTemp + SEA_ICE_OFFSET - 0.06, snowTemp + SEA_ICE_OFFSET + 0.02, seaTemp);
		float h = texture(waveHeight, uv*waveTiling).r;
		wave = h * waveAmp * calm;
		// Troughs may dip under radius 1: the planet draws no sea pixels.
		float d = wave;
		// Tessendorf choppy waves: shift the vertex horizontally along the
		// east/north tangents of the long/lat map (same basis as water.frag).
		// Shares the height's units, so choppiness 1 is the physical amount.
		vec2 D = texture(waveDisp, uv*waveTiling).rg * (waveAmp * waveChoppy * calm);
		vec3 T = normalize(vec3(-p.y, p.x, 1e-4));
		vec3 B = normalize(cross(p, T));
		disp = p * ((oceanOffset - 1.0) + d) + T * D.x + B * D.y;
	}

	// Row-vector pipeline convention (p * model), matching quad.vert. The
	// normal transform happens in water.frag straight from `model`. fSurfDir
	// stays the pre-displacement direction so the land-discard mask also
	// matches on walls.
	fSurfDir = p;
	fWave = wave;
	vec3 wp = sphere + patchOffset(disp, model);
	fragPos = wp;
	gl_Position = vec4(wp, 1.0) * view * projection;
}
