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
// downwind), and thin flat sheets (shelves) reach out from them. The map's
// walls are full cover built to the layer's top.
//
// Where the cover is full the shape is pulled toward solid, so big masses
// have no holes through them. Tall layers can stretch
// their shapes vertically (stretch) and flute their faces with columns
// (ridges). Streaks: thin horizontal bands, very long along the wind and
// stacked through the layer's height, densest round the walls: the
// windswept lines. The edge detail is swept along the wind too. The
// bottoms are hidden in the provider's deck (cloudvolume.frag).
//
// The map is read at a point that wanders with height, so walls bulge out
// and lean in instead of rising straight; the cover thins toward a
// cloud's top, so masses round off. A carpet (per layer) fills the
// layer's lowest part with low billows wherever the map has none, so the
// valleys between the masses have cloud in them.

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
uniform float layerRidges[MAX_LAYERS];
uniform float layerStretch[MAX_LAYERS];
uniform float layerStreaks[MAX_LAYERS];
uniform float layerCarpet[MAX_LAYERS];
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
	float edge = smoothstep(0.0, 0.12, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	if (edge <= 0.0)
		return 0.0;
	// The walls wander with height: the map is read up to 0.8 of the
	// lowest layer's shape period away, the offset turning over ~3 periods
	// of height and ~10 across.
	float ws = layerScale[layerCount - 1];
	vec3 wq = vec3(xy / (ws * 10.0), a / (ws * 3.0));
	vec2 wv = vec2(textureLod(shapeNoise, wq, 0.0).r, textureLod(shapeNoise, wq.yzx + 0.37, 0.0).r) - 0.5;
	vec2 uvw = uv + wv * (1.6 * ws / mapExtent);
	vec4 w = textureLod(weatherMap, uvw, 0.0);
	vec4 cw = textureLod(colorMap, uvw, 0.0);
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
		// The carpet: low billows over the layer's lowest part, in patches
		// (thicker and higher in some, thin in others) a few shape periods
		// across, swept with the wind.
		float carpet = 0.0, mapCover = cover;
		if (layerCarpet[i] > 0.0) {
			vec3 cq = vec3(xy.x / (layerSweep[i] * 2.0), xy.y, 0.0) / (sc * 3.0) + vec3(0.17, 0.41, 0.61);
			float pat = smoothstep(0.3, 0.75, textureLod(shapeNoise, cq, 0.0).r);
			// Flat-topped: its top varies little, and ends over a short span.
			float cTop = 0.25 + 0.06 * pat + 0.08 * (broad - 0.67);
			carpet = 1.0 - smoothstep(cTop - 0.06, cTop, hf);
			cover = max(cover, layerCarpet[i] * mix(0.6, 1.0, pat) * carpet);
		}
		if (cover <= 0.01)
			continue;
		// Lean: the shapes slide downwind with height, more (either way)
		// where the wind shears.
		float lean = layerShear[i] * (0.5 + 3.0 * shear);
		vec3 np = vec3((xy.x + (a - layerBase[i]) * lean) / layerSweep[i], xy.y, a / layerStretch[i]) / sc
		        + float(i) * vec3(0.37, 0.71, 0.13);
		float lodS = log2(max(foot / (sc / 128.0), 1.0));
		// Rounded tops, as high as the map's towering,
		// raised and lowered by broad cells (domes and valleys).
		// The cell noise falls off linearly from each cell's centre (a
		// cone); a circle profile makes it a dome. Walls go to the top.
		// Cells several times wider than the layer is tall, so the domes
		// are broad (narrow ones read as spires).
		float cell = 1.0 - mix(textureLod(shapeNoise, vec3(np.xy * 0.1, 0.5), 0.0).g,
		                       textureLod(shapeNoise, vec3(np.yx * 0.043 + 0.13, 0.81), 0.0).g, 0.45);
		float dome = sqrt(max(1.0 - 1.8 * cell * cell, 0.0));
		float topF = clamp(mix(0.45, 1.0, w.b) * (0.5 + 0.6 * dome), 0.2, 1.0);
		topF = max(topF, cw.a * (0.85 + 0.15 * dome));
		float hc = hf / topF;   // up the cloud, 0 at the base, 1 at its top
		// Anvil: where the wind shears, the top spreads and flattens and
		// the trunk below narrows; with the lean, the top overhangs.
		float anv = layerAnvil[i] * abs(shear);
		float cov = clamp(cover + anv * (0.4 * smoothstep(0.55, 0.9, hc) - 0.15 * (1.0 - hc)), 0.0, 1.0);
		// Thinning toward the top: masses round off and their sides slope.
		cov *= 1.0 - 0.45 * smoothstep(0.35, 1.0, hc);
		float prof = 1.0 - smoothstep(topF * mix(0.55, 0.9, anv), topF, hf);
		prof = max(prof, carpet);
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
		// Streaks: bands that change fast with height (several km apart)
		// and slowly along the wind (hundreds of km), their height
		// wandering with the broad noise so they bend; only round the
		// cloud masses (clear air gets none).
		float streak = 0.0;
		if (layerStreaks[i] > 0.0) {
			float env = smoothstep(0.15, 0.35, hf) * (1.0 - smoothstep(0.65, 0.9, hf))
			          * smoothstep(0.25, 0.7, cover);
			if (env > 0.0) {
				const float vf = 2.2;
				float wob = (broad - 0.67) * sc * 0.25;
				vec3 stp = vec3(xy.x / (layerSweep[i] * 6.0), xy.y * 1.5, (a + wob) * vf) / sc
				         + vec3(0.21, 0.63, 0.17 + 0.3 * float(i));
				float lodT = log2(max(foot * vf / (sc / 128.0), 1.0));
				float n = textureLod(shapeNoise, stp, lodT).r;
				streak = layerStreaks[i] * env * smoothstep(0.76, 0.9, n);
			}
		}
		if (prof <= 0.0 && sheet <= 0.0 && streak <= 0.0)
			continue;
		vec4 sn = textureLod(shapeNoise, np, lodS);
		float wfbm = sn.g * 0.625 + sn.b * 0.25 + sn.a * 0.125;
		// Perlin-Worley cut by the Worley cells, then stretched over its
		// actual range (about 0.5 to 0.95) so it carves the clouds.
		float shape = clamp((remap(sn.r, wfbm - 1.0, 1.0, 0.0, 1.0) - 0.5) / 0.45, 0.0, 1.0);
		// Full cover fills in (no holes through a mass), still varying, so
		// the erosion keeps carving its surface; less up top, where the
		// billows stay.
		shape = mix(shape, 1.0, 0.6 * smoothstep(0.6, 1.0, cover) * (1.0 - smoothstep(0.4, 0.8, hc)));
		// Ribs: cells nearly constant with height (columns), thinning the
		// shape between them, so wall faces are fluted top to bottom.
		if (layerRidges[i] > 0.0) {
			const float rf = 0.8;
			vec3 rp = vec3(np.xy * rf, np.z * 0.2) + vec3(0.5, 0.2, 0.7);
			float lodR = log2(max(foot * rf / (sc / 128.0), 1.0));
			float col = textureLod(shapeNoise, rp, lodR).g;
			shape *= mix(1.0, 0.3 + 0.7 * smoothstep(0.35, 0.7, col), layerRidges[i]);
		}
		// (cov can reach 0 here: the remap would divide by it.)
		float d = cov > 0.0 ? clamp(remap(shape * prof, 1.0 - cov, 1.0, 0.0, 1.0), 0.0, 1.0) * cov : 0.0;
		d = max(max(d, sheet), streak);
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
			// The carpet (where the map has no cloud) is eroded less: smoother.
			float er = layerErosion[i] * mix(1.0, 0.45, carpet * (1.0 - smoothstep(0.3, 0.7, mapCover)));
			d = clamp(remap(d, dmod * er * 0.6, 1.0, 0.0, 1.0), 0.0, 1.0);
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
