#version 410

// The light around a planet's clouds from its air (clouds.c, cloudvolume.frag
// under DOME_AIR), by the Sun's cosine at the point (texel x: -0.35 .. 1):
//
// - row 0, the sky: the mean radiance of the upper hemisphere at the
//   layer's middle, from the dome's own march (atmoscatter.glsl, converted
//   as the dome's light is), so the clouds' shaded sides take the sky's
//   colour: blue by day, the reds and the deepening blue of twilight, dark
//   once the air above has lost the Sun.
// - row 1, the ground: the light bounced up off ground of albedo
//   groundAlbedo, lit by the Sun through the air and by its own sky.
//
// Linear scene radiance, in the units of the clouds' sunColor (a white
// Lambert surface's radiance; x2 for thick cloud, as the march's Sun).

const float PI = 3.14159265359;

uniform float lutSize;         // texels across
uniform float layerAlt;        // m above fInnerRadius
uniform vec3 sunColor;         // linear, as the march's
uniform float sunAngle;        // rad
uniform vec3 groundAlbedo;     // linear
uniform float atmoHdrGain;     // Atmosphere_HdrGain
uniform float E;               // the dome's solar brightness
uniform float G_M;             // Mie asymmetry

out vec4 Light;

#include "world/atmosphere/atmosurface.glsl"
vec2 rayIntersection(vec3 p, vec3 dir, float radius)
{
	return shellIntersection(p, dir, radius);
}
// (No ring shadow on the sky's light.)
float scatterRingT(vec3 p, vec3 l)
{
	return 1.0;
}
#include "world/atmosphere/atmoscatter.glsl"
#include "engine/gfx/tonemap.glsl"

const int ELEV = 6;
const int AZIM = 8;

// The sky over a point at altitude alt with the Sun at cosine mus: its mean
// radiance over the upper hemisphere (x) and its cosine-weighted mean (y, a
// flat surface's irradiance / PI).
void skyMeans(float alt, vec3 l, out vec3 mean, out vec3 cosMean)
{
	vec3 p = vec3(0.0, fInnerRadius + alt, 0.0);
	float scaleH = 1.0 / fScaleHeight, scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	mean = vec3(0.0);
	cosMean = vec3(0.0);
	float wSum = 0.0;
	for (int k = 0; k < ELEV; k++) {
		// Even in z: even in solid angle.
		float z = (float(k) + 0.5) / float(ELEV);
		float s = sqrt(1.0 - z * z);
		for (int j = 0; j < AZIM; j++) {
			float phi = 2.0 * PI * (float(j) + 0.5) / float(AZIM);
			vec3 d = vec3(s * cos(phi), z, s * sin(phi));
			vec2 e = shellIntersection(p, d, fOuterRadius);
			e.x = max(e.x, 0.0);
			vec3 L = e.y > e.x ? sceneFromDisplay(inScatter(p, d, e, l, scaleH, scaleL)) * atmoHdrGain : vec3(0.0);
			mean += L;
			cosMean += L * z;
			wSum += z;
		}
	}
	mean /= float(ELEV * AZIM);
	cosMean /= wSum;
}

void main()
{
	float u = (gl_FragCoord.x - 0.5) / (lutSize - 1.0);
	float mus = mix(-0.35, 1.0, u);
	vec3 l = vec3(sqrt(max(1.0 - mus * mus, 0.0)), mus, 0.0);
	vec3 mean, cosMean;
	if (gl_FragCoord.y < 1.0) {
		skyMeans(layerAlt, l, mean, cosMean);
		Light = vec4(mean, 1.0);
	} else {
		skyMeans(0.0, l, mean, cosMean);
		vec3 sun = sunColor * sunTransmittance(vec3(0.0, fInnerRadius, 0.0), l, sunAngle) * max(mus, 0.0);
		Light = vec4(groundAlbedo * (sun + cosMean), 1.0);
	}
}
