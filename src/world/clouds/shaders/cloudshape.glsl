// The cloud layers' density (cloud_volume.c): shared by the march
// (cloudvolume.frag) and the shadow map (cloudshadow.frag). Needs
// tonemap.glsl included first.
//
// Per layer, the map's cover shapes Perlin-Worley noise under a height
// profile (soft base, rounded top as high as the map's "towering" says),
// stretched east-west (sweep) and leaning with height (shear); detail
// Worley noise erodes the edges, wispy low and billowy high (the Horizon
// Zero Dawn recipe). Where the map's wind shear is strong the clouds lean
// with it and their tops spread and flatten (anvils, which overhang
// downwind), and thin flat sheets (shelves) reach out from them; the
// detail is swept along the wind, so edges trail into streaks. The map's
// walls are full cover built to the layer's top.

#define MAX_LAYERS 2           // CLOUDVOL_MAX_LAYERS
uniform int layerCount;
uniform float layerBase[MAX_LAYERS];
uniform float layerTop[MAX_LAYERS];
uniform float layerDensity[MAX_LAYERS];
uniform float layerCoverage[MAX_LAYERS];
uniform float layerScale[MAX_LAYERS];
uniform float layerSweep[MAX_LAYERS];
uniform float layerShear[MAX_LAYERS];
uniform float layerErosion[MAX_LAYERS];
uniform float layerAnvil[MAX_LAYERS];
uniform float layerShelf[MAX_LAYERS];
uniform vec3 layerTint[MAX_LAYERS];

uniform float shellLo, shellHi;   // the layers' lowest base, highest top
uniform vec3 mapEast, mapNorth;   // body axes
uniform float mapExtent;          // m
uniform float detailScale;        // detail noise period / shape noise period

uniform sampler2D weatherMap;  // r top cover, g middle cover, b towering, a shear (0.5 none)
uniform sampler2D colorMap;    // rgb albedo (display), a walls
uniform sampler3D shapeNoise;
uniform sampler3D detailNoise;

float remap(float v, float lo, float hi, float nlo, float nhi)
{
	return nlo + (v - lo) / (hi - lo) * (nhi - nlo);
}

// Cloud extinction (1/m) at rel (body axes, m from the map's centre on the
// reference sphere), altitude a; foot: a pixel there (m), for the noise
// mips; detail: with the edge erosion. alb: the albedo there (linear).
float density(vec3 rel, float a, float foot, bool detail, out vec3 alb)
{
	alb = vec3(0.0);
	if (a <= shellLo || a >= shellHi)
		return 0.0;
	vec2 xy = vec2(dot(rel, mapEast), dot(rel, mapNorth));
	vec2 uv = xy / mapExtent + 0.5;
	float edge = smoothstep(0.0, 0.06, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	if (edge <= 0.0)
		return 0.0;
	vec4 w = textureLod(weatherMap, uv, 0.0);
	vec4 cw = textureLod(colorMap, uv, 0.0);
	float shear = w.a * 2.0 - 1.0;
	vec3 mapAlb = displayToLinear(cw.rgb);

	float total = 0.0;
	vec3 albSum = vec3(0.0);
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i >= layerCount)
			break;
		float hf = (a - layerBase[i]) / (layerTop[i] - layerBase[i]);
		if (hf <= 0.0 || hf >= 1.0)
			continue;
		float sc = layerScale[i];
		// Broad regions of thick cloud and wide valleys (~5 shape periods,
		// swept east-west), so the shape noise's tiling never shows.
		vec2 big = vec2(xy.x / (layerSweep[i] * 1.6), xy.y) / (sc * 5.3) + vec2(0.31, 0.77) * float(i);
		float broad = textureLod(shapeNoise, vec3(big, 0.37), 0.0).r;
		float cover = clamp((i == 0 ? w.r : w.g) * layerCoverage[i] + (broad - 0.67) * 1.6, 0.0, 1.0);
		cover = max(cover, cw.a);
		if (cover <= 0.01)
			continue;
		// Lean: the shapes slide downwind with height, more (either way)
		// where the wind shears.
		float lean = layerShear[i] * (0.5 + 3.0 * shear);
		vec3 np = vec3((xy.x + (a - layerBase[i]) * lean) / layerSweep[i], xy.y, a) / sc
		        + float(i) * vec3(0.37, 0.71, 0.13);
		float lodS = log2(max(foot / (sc / 128.0), 1.0));
		// Soft flat base; rounded tops, as high as the map's towering,
		// raised and lowered by broad cells (domes and valleys).
		// The cell noise falls off linearly from each cell's centre (a
		// cone); a circle profile makes it a dome. Walls go to the top.
		float cell = 1.0 - mix(textureLod(shapeNoise, vec3(np.xy * 0.5, 0.5), lodS).g,
		                       textureLod(shapeNoise, vec3(np.yx * 0.21 + 0.13, 0.81), lodS).g, 0.45);
		float dome = sqrt(max(1.0 - 1.8 * cell * cell, 0.0));
		float topF = clamp(mix(0.35, 1.0, w.b) * (0.15 + 0.95 * dome), 0.12, 1.0);
		topF = max(topF, cw.a * (0.85 + 0.15 * dome));
		float hc = hf / topF;   // up the cloud, 0 at the base, 1 at its top
		// Anvil: where the wind shears, the top spreads and flattens and
		// the trunk below narrows; with the lean, the top overhangs.
		float anv = layerAnvil[i] * abs(shear);
		float cov = clamp(cover + anv * (0.4 * smoothstep(0.55, 0.9, hc) - 0.15 * (1.0 - hc)), 0.0, 1.0);
		float prof = smoothstep(0.0, 0.1, hf) * (1.0 - smoothstep(topF * mix(0.55, 0.9, anv), topF, hf));
		// Shelves: thin sheets at two levels, tilted a little by the
		// broad noise, in long swept patches near the clouds.
		float sheet = 0.0;
		if (layerShelf[i] > 0.0) {
			float lv = hf + 0.08 * (broad - 0.67);
			float s1 = (lv - 0.34) / 0.06, s2 = (lv - 0.7) / 0.05;
			sheet = max(exp(-s1 * s1), 0.8 * exp(-s2 * s2));
			if (sheet > 0.02) {
				vec3 sp = vec3(xy.x / (layerSweep[i] * 4.0), xy.y, 0.0) / (sc * 1.3) + vec3(0.11, 0.53, 0.29 + 0.4 * float(i));
				float spread = smoothstep(0.66, 0.8, textureLod(shapeNoise, sp, lodS).r);
				sheet *= 0.6 * layerShelf[i] * spread * (0.35 + 0.65 * abs(shear)) * smoothstep(0.1, 0.5, cover);
			} else {
				sheet = 0.0;
			}
		}
		if (prof <= 0.0 && sheet <= 0.0)
			continue;
		vec4 sn = textureLod(shapeNoise, np, lodS);
		float wfbm = sn.g * 0.625 + sn.b * 0.25 + sn.a * 0.125;
		// Perlin-Worley cut by the Worley cells, then stretched over its
		// actual range (about 0.5 to 0.95) so it carves the clouds.
		float shape = clamp((remap(sn.r, wfbm - 1.0, 1.0, 0.0, 1.0) - 0.5) / 0.45, 0.0, 1.0);
		// (cov can reach 0 here: the remap would divide by it.)
		float d = cov > 0.0 ? clamp(remap(shape * prof, 1.0 - cov, 1.0, 0.0, 1.0), 0.0, 1.0) * cov : 0.0;
		d = max(d, sheet);
		if (d <= 0.0)
			continue;
		if (detail && layerErosion[i] > 0.0) {
			float lodD = log2(max(foot / (sc * detailScale / 32.0), 1.0));
			// Swept: stretched along the wind and squashed in height,
			// nudged by the shape noise, so eroded edges trail as streaks.
			vec3 dp = vec3(np.x / 2.5, np.y, np.z * 1.5) / detailScale
			        + vec3(sn.g - 0.5, sn.b - 0.5, 0.0) * 1.2;
			vec3 dn = textureLod(detailNoise, dp, lodD).rgb;
			float dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
			// Wispy at the base, billowy at the top.
			float dmod = mix(dfbm, 1.0 - dfbm, clamp(hc * 4.0, 0.0, 1.0));
			d = clamp(remap(d, dmod * layerErosion[i] * 0.6, 1.0, 0.0, 1.0), 0.0, 1.0);
		}
		d *= layerDensity[i] * edge;
		if (d <= 0.0)
			continue;
		total += d;
		albSum += d * mapAlb * layerTint[i];
	}
	if (total > 0.0)
		alb = albSum / total;
	return total;
}
