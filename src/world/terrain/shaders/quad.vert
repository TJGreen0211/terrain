#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
// Quadtree patch instance (locations 4-12) + patchDir/patchSphere.
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
#include "terrainlift.glsl"

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

	// Relief and land detail (terrainlift.glsl), then laid onto a coarser
	// neighbour's edge where there is one (terrainLiftStitched).
	float vDist = length(sphere - lodCameraPos);
	vec3 q = patchCube(uv);
	vec2 gradCube;
	float lift = terrainLift(d, vDist, q, pAddr, uv, gradCube);
	fDetailSlope = detailSlope(pAddr.x, q, gradCube);
	fPatchAddr = pAddr;
	fPatchUV = uv;
	lift = terrainLiftStitched(uv, lift);
	fShadowPos = sphere + patchOffset(d * lift, model);

	// Boundary skirt: wall vertices (w = 1) sit just under the DISPLACED
	// planet surface so they hide T-junction seams along LOD patch edges
	// without z-fighting it (the lift is a function of the surface point
	// only, so the skirt follows the surface across the whole edge). The drop is
	// sized per patch from its height range (patchSkirt); kept as the small
	// offset from 1 so it never rounds against the unit radius.
	if(vPosition.w > 0.5)
		lift -= patchSkirt();
	float radius = 1.0 + lift;

	vec3 world = sphere + patchOffset(d * lift, model);

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
