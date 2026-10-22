#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
// Quadtree patch instance (locations 4-11) + patchDir/patchSphere.
#include "patch.glsl"
// Terrain detail noise (step 3b).
#include "detail.glsl"

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;
// Baked height field (terrain_gen.c): the land is geometrically displaced
// above the unit sphere and the seafloor below it along a squared shelf
// profile, so sea level is exactly radius 1 where the water shell sits and
// vertices near the coast stay close to it. lodK/lodCameraPos drive the vertex texture-fetch
// LOD, combined with the quadtree node's cell size so the geometry follows
// the field at its own resolution and never crinkles finer than it can draw.
uniform samplerCube heightMap;
uniform samplerCube surfaceMap;   // slope: sizes the detail (detailEnvelope)
#include "planetmap.glsl"
uniform float seaLevel;
uniform float landRelief;
uniform float oceanRelief;
uniform float oceanDepth;
uniform vec3 lodCameraPos;
uniform float lodK;
// Mip by vertex distance, as wide as the widest patch the walk can have
// there (terrain_draw.c lodCellKFor): continuous across patch levels.
uniform float lodCellK;

// The depth prepass and the colour pass are separate programs built from
// this source; their depths must match exactly (colour pass tests >=).
invariant gl_Position;

out vec3 fragPos;
out vec3 fE;
out vec3 fN;
out vec3 fL;
out vec2 fTexCoords;
out vec3 vColor;
out vec3 fSurfDir;
// Interpolated geometric radius (sea level = 1): quad.frag pushes seafloor
// that coarse triangles lift above the water back behind the shell.
out float fRadius;
out mat3 fNormalMat;
// Terrain detail: the patch address and uv for quad.frag's per-pixel
// octaves, and the slope of the octaves the vertices carry (body-local,
// m/m; detail.glsl).
flat out ivec4 fPatchAddr;
out vec2 fPatchUV;
out vec3 fDetailSlope;
// Where this fragment takes its shadow from: the surface point, also for
// skirt walls (the edge they hang from). A wall glimpsed through a patch-
// edge crack lies under the neighbouring surface, and the fine shadow
// cascades would otherwise draw it as a dark line along every edge.
out vec3 fShadowPos;

const float PI = 3.14159265359;

void main()
{
	vec2 uv = vPosition.xy;
	vec3 d = patchDir(uv);
	// Camera-relative sphere point, built from the patch corners (patch.glsl).
	vec3 sphere = patchSphere(uv, d, model);

	// Vertex texture fetch of the shaping field at the resolution of the
	// patches at this distance (never finer than a pixel footprint either).
	float vDist = length(sphere - lodCameraPos);
	// (The face-corner bias goes on before the clamp to mip 0.)
	float lodCell = log2(max(vDist*lodCellK, 1e-30));
	float lodPix = log2(max(vDist*lodK, 1e-30));
	float lod = clamp(max(lodCell, lodPix) + planetMapLodBias(d), 0.0, planetMapMaxLod(heightMap));
	float h = textureLod(heightMap, d, lod).r;

	// Relief as a fraction of the planet radius (model scale): a linear ramp
	// from sea level to the field maximum on land, and a squared shelf down
	// to oceanRelief at oceanDepth on the seafloor (water.frag's column
	// thickness uses the same profile).
	float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
	float relief = landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
	             - oceanRelief * s * s;

	// Boundary skirt: wall vertices (w = 1) sit just under the DISPLACED
	// planet surface so they hide T-junction seams along LOD patch edges
	// without z-fighting it (relief and detail are functions of the surface
	// point only, so the skirt follows the surface across the whole edge). The drop is
	// sized per patch from its height range (patchSkirt); kept as the small
	// offset from 1 so it never rounds against the unit radius.
	// Detail noise on land, faded by this vertex's distance (the same for
	// every patch sharing it, so no cracks), in metres -> body radii.
	float alt01 = max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3);
	float mapSlope = h > seaLevel ? length(textureLod(surfaceMap, d, lod).xyz) : 0.0;
	float env = detailEnvelope(landRelief * alt01 * detailRadius, mapSlope);
	vec3 q = patchCube(uv);
	vec2 gradCube;
	float detail = detailVertex(pAddr, uv, length(q),
	                            vDist * detailPix, env, gradCube);
	fDetailSlope = detailSlope(pAddr.x, q, gradCube);
	fPatchAddr = pAddr;
	fPatchUV = uv;

	float lift = relief + detail / detailRadius;
	if(vPosition.w > 0.5)
		lift -= patchSkirt();
	float radius = 1.0 + lift;

	vec3 world = sphere + patchOffset(d * lift, model);
	fShadowPos = sphere + patchOffset(d * (relief + detail / detailRadius), model);

	vec3 n = d;
	fSurfDir = d;
	fRadius = radius;

	// Normal matrix: this pipeline transforms as row vectors (p * model * view
	// * projection) from row-major uploads, so the GLSL matrix is the
	// transposed model. Applying the standard transpose(inverse(M)) here would
	// rotate normals by the planet's INVERSE rotation (day/night terminator
	// drifting off the surface); inverse(M) alone yields the correct world
	// normal under this convention.
	mat3 normalMatrix = inverse(mat3(model));
	fNormalMat = normalMatrix;
	vec3 lightDir = normalize(world - lightPosition);
	fE = -normalize(world);
	fN = normalize(n*normalMatrix);
	fL = -lightDir;

	fragPos = world;
	fTexCoords = (d.xy*radius+1.0)/2.0;

	gl_Position = vec4(world, 1.0)*view*projection;

	vColor = vTangent;//vec3(1.0, 0.5, 0.2);
}
