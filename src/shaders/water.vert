#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
layout (location = 4) in mat4 vInstanceMatrix;

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
uniform float oceanRelief;
uniform vec3 lodCameraPos;
uniform float lodK;

const float PI = 3.14159265359;
// Normalized depth over which waves ramp from flat (coast) to full height.
const float SHORE_CALM = 0.25;

out vec3 fSurfDir;
out vec3 fragPos;
out float fWave;

void main()
{
	// Match quad.vert: instance-transform the face grid, normalize to the unit
	// sphere, then the model (scale * planet rotation) maps to the world.
	vec4 fPosition = vec4(vPosition.xyz, 1.0);
	vec3 transformed = (fPosition*vInstanceMatrix).xyz;
	vec3 p = (length(transformed) > 0.0001) ? normalize(transformed) : vec3(0.0);

	vec3 disp;
	float wave;
	if(vPosition.w > 0.5) {
		// Boundary skirt: wall vertices skip the wave displacement and sit
		// just under sea level, hiding LOD patch-edge cracks under the shell.
		disp = p * (oceanOffset - 0.001);
		wave = 0.0;
	}
	else {
		// Long/lat UV computed from the direction; the DFT wave texture is
		// periodic, so the u = 0/1 seam is continuous.
		vec2 uv = vec2((atan(p.y, p.x) / 3.1415926 + 1.0) * 0.5,
		               (asin(p.z) / 3.1415926 + 0.5));
		// Water depth under this vertex, fetched like quad.vert so the
		// shell and the seafloor agree on the coastline.
		float cell = vInstanceMatrix[0].w;
		float lodCell = log2(max(cell*2048.0/PI, 1.0));
		float lodPix = log2(max(length(vec4(p, 1.0)*model - vec4(lodCameraPos, 0.0))*lodK, 1e-4));
		float th = textureLod(heightMap, uv, clamp(max(lodCell, lodPix), 0.0, 11.0)).r;
		float depthT = clamp((seaLevel - th) / max(oceanDepth, 1e-4), 0.0, 1.0);
		// Waves die out toward the shore: at the coastline the shell sits
		// exactly at sea level, where the planet's land relief starts.
		float calm = smoothstep(0.0, SHORE_CALM, depthT);
		float h = texture(waveHeight, uv*waveTiling).r;
		wave = h * waveAmp * calm;
		// Keep troughs above the (now sunken) seafloor.
		float d = max(wave, -0.5 * oceanRelief * depthT);
		// Tessendorf choppy waves: shift the vertex horizontally along the
		// east/north tangents of the long/lat map (same basis as water.frag).
		// Shares the height's units, so choppiness 1 is the physical amount.
		vec2 D = texture(waveDisp, uv*waveTiling).rg * (waveAmp * waveChoppy * calm);
		vec3 T = normalize(vec3(-p.y, p.x, 1e-4));
		vec3 B = normalize(cross(p, T));
		disp = p * (oceanOffset + d) + T * D.x + B * D.y;
	}

	// Row-vector pipeline convention (p * model), matching quad.vert. The
	// normal transform happens in water.frag straight from `model`. fSurfDir
	// stays the pre-displacement direction so the land-discard mask also
	// matches on walls.
	fSurfDir = p;
	fWave = wave;
	vec4 wp = vec4(disp, 1.0) * model;
	fragPos = wp.xyz;
	gl_Position = wp * view * projection;
}
