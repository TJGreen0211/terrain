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
uniform float oceanOffset;
uniform float waveAmp;
uniform float waveTiling;

out vec3 fSurfDir;
out vec3 fragPos;
out mat3 fNormalMat;
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
		// below the trough floor (1.005) and above the planet (1.0), hiding
		// LOD patch-edge cracks under the shell.
		disp = p * 1.004;
		wave = 0.0;
	}
	else {
		// Long/lat UV computed from the direction; the DFT wave texture is
		// periodic, so the u = 0/1 seam is continuous.
		vec2 uv = vec2((atan(p.y, p.x) / 3.1415926 + 1.0) * 0.5,
		               (asin(p.z) / 3.1415926 + 0.5));
		float h = texture(waveHeight, uv*waveTiling).r;
		wave = h * waveAmp;
		// Floor the trough so the shell can never dip under the planet surface.
		float d = max(wave, 1.005 - oceanOffset);
		disp = p * (oceanOffset + d);
	}

	// Row-vector pipeline convention (p * model), matching quad.vert:
	// the normal matrix is inverse(mat3(model)). fSurfDir stays the pre-
	// displacement direction so the land-discard mask also matches on walls.
	mat3 normalMatrix = inverse(mat3(model));

	fSurfDir = p;
	fWave = wave;
	fNormalMat = normalMatrix;
	vec4 wp = vec4(disp, 1.0) * model;
	fragPos = wp.xyz;
	gl_Position = wp * view * projection;
}
