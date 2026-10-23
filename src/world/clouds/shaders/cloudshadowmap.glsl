// Reads the cloud layers' shadow map (cloud_volume.c: CloudVolume_BindShadow
// sets these uniforms on any program). For the air around and below the
// clouds: sunlight through the gaps between them makes shafts.
//
// A point is carried along the Sun to the shell's floor (up or down; flat,
// as the map was made), and the depth above its height is read between the
// map's three levels. Outside the map, or with no map, there is no shadow.

uniform sampler2D cloudShadowMap;
uniform int csOn;
uniform mat3 csViewToBody;
uniform vec3 csCamToMap;       // camera - the map's centre, body axes, m
uniform vec3 csEast, csNorth;  // the map plane, body axes
uniform vec2 csCenter;         // the shadow map's centre in it, m
uniform float csExtent;        // m
uniform float csLo, csHi;      // the shell, m above the reference radius
uniform vec3 csSun;            // toward the Sun, body axes

// The clouds' optical depth toward the Sun from view-space point pv at
// altitude a, where the Sun's cosine above the horizon is mus.
float cloudShadowTau(vec3 pv, float a, float mus)
{
	if (csOn == 0 || a >= csHi)
		return 0.0;
	vec3 rel = csCamToMap + csViewToBody * pv;
	vec3 base = rel - csSun * ((a - csLo) / max(mus, 0.1));
	vec2 uv = (vec2(dot(base, csEast), dot(base, csNorth)) - csCenter) / csExtent + 0.5;
	float edge = smoothstep(0.0, 0.08, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	if (edge <= 0.0)
		return 0.0;
	vec3 t = textureLod(cloudShadowMap, uv, 0.0).rgb;
	float h = clamp((a - csLo) / (csHi - csLo), 0.0, 1.0) * 3.0;
	float tau = h < 1.0 ? mix(t.r, t.g, h) : h < 2.0 ? mix(t.g, t.b, h - 1.0) : mix(t.b, 0.0, h - 2.0);
	return tau * edge;
}

float cloudShadow(vec3 pv, float a, float mus)
{
	return exp(-cloudShadowTau(pv, a, mus));
}
