#version 410

in vec3 fragPos;
in vec3 fN;
in vec3 fAlbedo;
in float fHeight;
in float fTau;

out vec4 FragColor;

uniform vec3 lightPosition;   // camera-relative
uniform vec3 ringCenter;      // camera-relative planet centre
uniform vec3 ringNormal;
uniform float planetRadius;
uniform float ringSunAngle;
uniform float halfThickness;

const float PI = 3.14159265359;

// Planet shadow (same as ring.frag): p relative to the planet centre.
float planetShadow(vec3 p, vec3 l)
{
	float b = -dot(p, l);
	if (b <= 0.0)
		return 1.0;
	float d = length(p + l * b);
	float w = planetRadius * 0.01 + b * ringSunAngle;
	return smoothstep(planetRadius - w, planetRadius + w, d);
}

// Sunlight reaching a rock inside the slab has crossed the part of the
// ring between it and the sun-facing surface, so rocks on the shadowed face
// are darker. Quarter strength: the sheet's optical depth includes the
// rocks themselves.
float slabTransmittance(vec3 l)
{
	float ln = dot(l, ringNormal);
	float depth = clamp((halfThickness - fHeight * sign(ln)) / (2.0 * halfThickness), 0.0, 1.0);
	return exp(-fTau * depth * 0.25 / max(abs(ln), 0.05));
}

void main()
{
	vec3 V = normalize(-fragPos);
	// Faced toward the viewer, so the meshes' winding does not matter.
	vec3 N = normalize(fN);
	if (dot(N, V) < 0.0)
		N = -N;
	vec3 L = normalize(lightPosition - fragPos);
	vec3 H = normalize(L + V);

	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;   // matches quad.frag
	float planetSun = planetShadow(fragPos - ringCenter, L);
	float sun = planetSun * slabTransmittance(L);
	float NdotL = max(dot(N, L), 0.0);
	float spec = pow(max(dot(N, H), 0.0), 40.0) * 0.08 * NdotL;   // icy sheen
	// Ringshine: sunlight scattered by the surrounding ring lights the
	// rocks from every side (roughly the sheet's radiance over half the sky).
	vec3 ringshine = fAlbedo * sunRadiance * 0.06 * planetSun * min(fTau, 1.0);
	vec3 lit = fAlbedo * 0.02 + ringshine + (fAlbedo / PI * NdotL + spec) * sunRadiance * sun;

	// Same filmic tone map + gamma as quad.frag.
	float exposure = 1.05;
	vec3 x = lit * exposure;
	lit = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	FragColor = vec4(pow(lit, vec3(1.0 / 2.2)), 1.0);
}
