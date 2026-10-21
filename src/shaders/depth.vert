#version 410

// Shadow-map caster pass. Positions go through the same camera-relative
// model matrix the main pass uses, then the light's projection * view
// (lightSpace). Hardware depth is what gets stored; receivers compare their
// own lightSpace-projected depth against it (see sunVisibility in quad.frag).
layout (location = 0) in vec4 vPosition;
// Quadtree patch instance (locations 4-10) + patchDir/patchSphere.
#include "patch.glsl"

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
uniform sampler2D heightMap;
uniform float seaLevel;
uniform float landRelief;
uniform float oceanRelief;
uniform float oceanDepth;
uniform vec3 lodCameraPos;
uniform float lodK;

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
			// Identical to quad.vert: node-cell lod vs pixel-footprint lod.
			float cell = patchCell();
			vec2 huv = vec2((atan(d.y, d.x)/PI + 1.0)*0.5,
			                (asin(clamp(d.z, -1.0, 1.0))/PI + 0.5));
			float lodCell = log2(max(cell*2048.0/PI, 1.0));
			float lodPix = log2(max(length(sphere - lodCameraPos)*lodK, 1e-4));
			float lod = clamp(max(lodCell, lodPix), 0.0, 11.0);
			float h = textureLod(heightMap, huv, lod).r;
			float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
			lift = landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
			     - oceanRelief * s * s;
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
