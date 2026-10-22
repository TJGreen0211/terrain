#version 410

// Shadow-map caster pass. Positions go through the same camera-relative
// model matrix the main pass uses, then the light's projection * view
// (lightSpace). Hardware depth is what gets stored; receivers compare their
// own lightSpace-projected depth against it (see sunVisibility in quad.frag).
layout (location = 0) in vec4 vPosition;
// Quadtree patch instance (locations 4-11) + patchDir/patchSphere.
#include "patch.glsl"
// Terrain detail noise (step 3b), as in quad.vert.
#include "detail.glsl"

uniform mat4 model;
uniform mat4 lightSpace;
// 1 = quadtree LOD instance (QuadTree_DrawShadowCasters): the patch vertex
// is placed exactly like quad.vert (patch.glsl), so the caster is the very
// mesh the planet pass draws and the terrain does not self-shadow against a
// mismatched surface.
uniform int instanced;
// Geometric relief (see quad.vert), only for the planet: same height field,
// same lod selection, same skirt drop. Moon geometry stays undisplaced.
uniform float displace;
uniform samplerCube heightMap;
uniform samplerCube surfaceMap;   // as quad.vert
#include "planetmap.glsl"
uniform float seaLevel;
uniform float landRelief;
uniform float oceanRelief;
uniform float oceanDepth;
uniform vec3 lodCameraPos;
uniform float lodK;
uniform float lodCellK;   // as quad.vert (the main walk's value)

const float PI = 3.14159265359;

void main()
{
	vec3 world;
	if (instanced == 1) {
		vec2 uv = vPosition.xy;
		vec3 d = patchDir(uv);
		vec3 sphere = patchSphere(uv, d, model);
		float lift = 0.0;
		if (displace > 0.5) {
			// Identical to quad.vert: patch-size lod vs pixel-footprint lod,
			// both by vertex distance.
			float vDist = length(sphere - lodCameraPos);
			float lodCell = log2(max(vDist*lodCellK, 1e-30));
			float lodPix = log2(max(vDist*lodK, 1e-30));
			float lod = clamp(max(lodCell, lodPix) + planetMapLodBias(d), 0.0, planetMapMaxLod(heightMap));
			float h = textureLod(heightMap, d, lod).r;
			float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
			lift = landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
			     - oceanRelief * s * s;
			// Detail as quad.vert, faded for this walk's coarser triangles
			// (detailTriPx = QT_SHADOW_TRIANGLE_PX).
			float alt01 = max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3);
			float mapSlope = h > seaLevel ? length(textureLod(surfaceMap, d, lod).xyz) : 0.0;
			float env = detailEnvelope(landRelief * alt01 * detailRadius, mapSlope);
			vec2 gradCube;
			lift += detailVertex(pAddr, uv, length(patchCube(uv)),
			                     vDist * detailPix, env, gradCube) / detailRadius;
		}
		// Skirt: same drop as quad.vert, as an offset from 1.
		if (vPosition.w > 0.5)
			lift = lift - patchSkirt();
		world = sphere + patchOffset(d * lift, model);
	}
	else {
		world = (vec4(vPosition.xyz, 1.0)*model).xyz;
	}
	gl_Position = vec4(world, 1.0)*lightSpace;
}
