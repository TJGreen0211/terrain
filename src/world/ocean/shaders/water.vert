#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
// Quadtree patch instance (locations 4-11) + patchDir/patchSphere.
#include "world/terrain/patch.glsl"

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
// FFT cascades (Water.c), one layer each, largest first.
uniform sampler2DArray waveHeight;
uniform sampler2DArray waveDisp;    // choppy horizontal displacement (face axes)
uniform float waveChoppy;
uniform float oceanOffset;
uniform float waveAmp;
#include "wavecube.glsl"
// Each cascade's patches per body radius at its drawn size, and that size
// over true size (its heights scale with it; the finest stays at 1).
uniform float waveTilings[WAVE_CASCADES];
uniform float waveCascadeScales[WAVE_CASCADES];
// Geometric displacement scale: the wave field is in body radii (rms
// OCEAN_FFT_RMS per unit waveAmp); this maps it to OCEAN_SWELL_RMS_M of
// real swell (WorldConfig.h), before each cascade's waveCascadeScales.
// fWave (foam, crest shading) stays unscaled.
uniform float waveGeomScale;
// Same height field / sea level / LOD inputs as quad.vert, so the shell knows
// the water depth under each vertex and calms the waves toward the coast.
uniform samplerCube heightMap;
#include "world/terrain/planetmap.glsl"
uniform float seaLevel;
uniform float oceanDepth;
uniform float oceanRelief;     // seafloor depth at depthT 1, body radii
uniform float planetRadiusM;
uniform vec3 lodCameraPos;
uniform float lodK;

// Sea state (TerrainGen.c bakeOcean): R wind speed / windMax, A fetch.
uniform samplerCube oceanMap;
uniform float windMax;
// Planet temperature response (g_planetStyle), for the sea-ice mask.
uniform float tempOffset;
uniform float snowTemp;
// Per-cascade wave strength over the planet (same as water.frag).
#include "seastate.glsl"
// Waves running in along the coast (as water.frag).
uniform float time;
#include "shorewaves.glsl"

const float PI = 3.14159265359;
// Mesh spacing in pixels: the water walk keeps triangle edges between
// QT_WATER_TRIANGLE_PX / 2 and QT_WATER_TRIANGLE_PX (24-48 px).
const float WAVE_VERT_PX = 32.0;

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
		disp = p * ((oceanOffset - 1.0) - patchSkirt());
		wave = 1000.0;
	}
	else {
		// Mesh spacing here (body radii) from the vertex's distance alone:
		// the pixel footprint (dist * lodK, in map texels) times the
		// triangle size. Every mip below is chosen from it, not from the
		// patch's own cell size: neighbouring patches at different levels
		// share their edge vertices, and per-patch mips gave those vertices
		// a different wave height on each side, opening cracks along every
		// LOD seam.
		float texel = planetMapTexel(heightMap);
		float cell = length(sphere - lodCameraPos) * lodK * texel * WAVE_VERT_PX;
		// Water depth under this vertex, so the shell calms toward the
		// coast where the seafloor rises.
		float mapLod = clamp(log2(max(cell / texel, 1.0)) + planetMapLodBias(p),
		                     0.0, planetMapMaxLod(heightMap));
		float th = planetMapFine(heightMap, p, mapLod, seaCoastBand());
		float depthT = clamp((seaLevel - th) / max(oceanDepth, 1e-4), 0.0, 1.0);
		// Sea state (seastate.glsl): waves die out toward the shore (at the
		// coastline the shell sits exactly at sea level, where the planet's
		// land relief starts), follow the baked wind and fetch (calm
		// doldrums and lee coasts, heavy seas under the westerlies), vary
		// with swell trains and gusts, and lie flat under the sea ice.
		vec4 sea = textureLod(oceanMap, p, mapLod);
		float iceCalm = seaIceCalm(p);
		vec3 gain = waveCascadeGains(depthT, sea.r * windMax, sea.a,
		                             iceCalm, seaGust(p, cell), seaSwell(p));
		// Shore waves (shorewaves.glsl, gated as water.frag): their crests
		// lift the shell once the mesh resolves them, close up.
		float shoreH = 0.0;
		float shoreGate = (1.0 - smoothstep(1.0, 2.0, seaDepthM(depthT) / max(waveShoreDepths[0], 1e-3))) * iceCalm;
		if (shoreGate > 0.0) {
			vec3 T = normalize(vec3(-p.y, p.x, 1e-4));
			vec3 B = normalize(cross(p, T));
			vec3 seaward;
			float coastM = shoreCoast(p, th, mapLod, T, B, seaward);
			float U = sea.r * windMax;
			float surfStrength = mix(0.2, 1.0, smoothstep(0.1, 0.6, sea.a) * smoothstep(2.0, 8.0, U));
			shoreH = shoreWaves(coastM, p, surfStrength * shoreGate, cell * planetRadiusM).height;
		}
		// Wave field on the cube faces (wavecube.glsl, as water.frag), one
		// layer per cascade. The mip matches the vertex spacing, so where a
		// cascade's patches are far smaller than the mesh (from orbit, all
		// of them) it filters out instead of aliasing into a coarse,
		// crawling bumpiness.
		float texCells = cell * float(textureSize(waveHeight, 0).x);
		vec3 fw = waveFaceWeights(p);
		float h = 0.0;    // drawn height (cascade scales applied)
		float hs = 0.0;   // field height, for the crest shading (fWave)
		vec3 D = vec3(0.0);
		for (int k = 0; k < 3; k++) {
			if (fw[k] <= 0.0)
				continue;
			vec3 E0, E1;
			vec2 q = waveFace(k, p, E0, E1);
			vec2 Dq = vec2(0.0);
			for (int c = 0; c < WAVE_CASCADES; c++) {
				// Rotated and shifted per face and cascade (wavecube.glsl).
				mat2 R = waveCascadeRot(k, c);
				vec3 uv = vec3(R * (q * waveTilings[c]) + waveCascadeOffset(k, c), float(c));
				float lod = log2(max(texCells * waveTilings[c], 1.0));
				float a = gain[c] * waveCascadeScales[c];
				float hc = fw[k] * gain[c] * textureLod(waveHeight, uv, lod).r;
				hs += hc;
				h += hc * waveCascadeScales[c];
				Dq += a * (textureLod(waveDisp, uv, lod).rg * R);
			}
			// Tessendorf choppy waves: shift the vertex horizontally
			// along the face's map axes on the sphere.
			D += fw[k] * (normalize(E0) * Dq.x + normalize(E1) * Dq.y);
		}
		float norm = waveFaceNorm(fw);
		wave = hs * norm * waveAmp;
		// Troughs may dip under radius 1: the planet draws no sea pixels.
		float d = h * norm * waveAmp;
		// Shares the height's units, so choppiness 1 is the physical amount.
		D *= norm * waveAmp * waveChoppy;
		disp = p * ((oceanOffset - 1.0) + d * waveGeomScale + shoreH / planetRadiusM) + D * waveGeomScale;
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
