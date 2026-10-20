#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
layout (location = 4) in mat4 vInstanceMatrix;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;
// Baked height field (TerrainGen.c): the land is geometrically displaced
// above the unit sphere and the ocean floor below it, so sea level is
// exactly radius 1 where the water shell sits. lodK/lodCameraPos drive the vertex texture-fetch
// LOD, combined with the quadtree node's cell size so the geometry follows
// the field at its own resolution and never crinkles finer than it can draw.
uniform sampler2D heightMap;
uniform float seaLevel;
uniform float landRelief;
uniform float oceanRelief;
uniform float oceanDepth;
uniform vec3 lodCameraPos;
uniform float lodK;

out vec4 fLightSpace;
out vec3 fragPos;
out vec3 fE;
out vec3 fN;
out vec3 fL;
out vec2 fTexCoords;
out vec3 vColor;
out vec3 fSurfDir;
// Interpolated geometric radius (sea level = 1): quad.frag compares it with
// the per-pixel height to hide seafloor that coarse triangles lift above the
// water shell.
out float fRadius;
out mat3 fNormalMat;

const float PI = 3.14159265359;

void main()
{
	vec4 fPosition = vec4(vPosition.xyz, 1.0);

	vec3 transformed = (fPosition*vInstanceMatrix).xyz;
	vec3 d = (length(transformed) > 0.0001) ? normalize(transformed) : vec3(0.0);

	// Vertex texture fetch of the shaping field at the node's own resolution
	// (never finer than a pixel footprint either). The quadtree walk packs
	// the node's face cell half-width into instance matrix m[3][0], which is
	// column 0 row 3 under this row-vector layout (QuadTree.c).
	float cell = vInstanceMatrix[0].w;
	vec2 huv = vec2((atan(d.y, d.x)/PI + 1.0)*0.5,
	                (asin(clamp(d.z, -1.0, 1.0))/PI + 0.5));
	float lodCell = log2(max(cell*2048.0/PI, 1.0));
	float lodPix = log2(max(length(vec4(d, 1.0)*model - vec4(lodCameraPos, 0.0))*lodK, 1e-4));
	float lod = clamp(max(lodCell, lodPix), 0.0, 11.0);
	float h = textureLod(heightMap, huv, lod).r;

	// Linear relief ramps from sea level (radius 1): up to the field maximum
	// on land, down to oceanDepth on the seafloor (fraction of planet radius).
	float relief = landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
	             - oceanRelief * clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
	float radius = 1.0 + relief;

	// Boundary skirt: wall vertices (w = 1) sit just under the DISPLACED
	// planet surface so they hide T-junction seams along LOD patch edges
	// without z-fighting it (the relief is a function of direction only, so
	// the skirt follows the surface across the whole edge).
	if(vPosition.w > 0.5)
		radius *= 0.995;

	fPosition.xyz = d * radius;

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
	vec4 lightPos = vec4(lightPosition, 1.0);
	vec3 lightDir = normalize(fPosition*model - lightPos).xyz;
	fE = -normalize(fPosition*model).xyz;
	fN = normalize(n*normalMatrix);
	fL = -normalize(lightDir);

	// fPosition already carries the instance transform, so applying
	// vInstanceMatrix again would double-transform the shadow lookup.
	fLightSpace = fPosition*model;
	fragPos = vec3(fPosition*model).xyz;
	fTexCoords = (fPosition.xy+1.0)/2.0;//vTexCoords*mat2(vInstanceMatrix);

	gl_Position = fPosition*model*view*projection;

	vColor = vTangent;//vec3(1.0, 0.5, 0.2);
}
