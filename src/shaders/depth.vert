#version 410

// Shadow-map caster pass. Positions go through the same camera-relative
// model matrix the main pass uses, then the light's projection * view
// (lightSpace). Hardware depth is what gets stored; receivers compare their
// own lightSpace-projected depth against it (see sunVisibility in quad.frag).
layout (location = 0) in vec4 vPosition;
layout (location = 4) in mat4 vInstanceMatrix;

uniform mat4 model;
uniform mat4 lightSpace;
// 1 = quadtree LOD instance (QuadTree_DrawShadowCasters): the face-grid
// vertex is instance-transformed and normalized onto the unit sphere exactly
// like quad.vert, so the caster is the very mesh the planet pass draws and
// the terrain does not self-shadow against a mismatched surface.
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
	vec3 p = vPosition.xyz;
	if (instanced == 1) {
		vec3 transformed = (vec4(vPosition.xyz, 1.0)*vInstanceMatrix).xyz;
		vec3 d = (length(transformed) > 0.0001) ? normalize(transformed) : vec3(0.0);
		float radius = 1.0;
		if (displace > 0.5) {
			// Identical to quad.vert: node-cell lod vs pixel-footprint lod.
			float cell = vInstanceMatrix[0].w;
			vec2 huv = vec2((atan(d.y, d.x)/PI + 1.0)*0.5,
			                (asin(clamp(d.z, -1.0, 1.0))/PI + 0.5));
			float lodCell = log2(max(cell*2048.0/PI, 1.0));
			float lodPix = log2(max(length(vec4(d, 1.0)*model - vec4(lodCameraPos, 0.0))*lodK, 1e-4));
			float lod = clamp(max(lodCell, lodPix), 0.0, 11.0);
			float h = textureLod(heightMap, huv, lod).r;
			float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
			radius += landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
			        - oceanRelief * s * s;
		}
		if (vPosition.w > 0.5)
			radius *= 0.995;
		p = d * radius;
	}
	gl_Position = vec4(p, 1.0)*model*lightSpace;
}
