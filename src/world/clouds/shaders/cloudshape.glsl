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
uniform float layerPlume[MAX_LAYERS];
uniform vec3 layerTint[MAX_LAYERS];
uniform float plumeLo, plumeHi;   // m: the band a plume can be in
uniform int plumeLayer;           // the layer with plumes (when plumeLo is reachable)

uniform float shellLo, shellHi;   // the layers' lowest base, highest top
uniform vec3 mapEast, mapNorth;   // body axes
// The noise's own frame (fixed on the body while the map re-centres under
// the camera, so the clouds stay put): a point's offset from its centre is
// its offset from the map's plus noiseShift.
uniform vec3 noiseShift;          // map centre - noise centre, body axes, m
uniform vec3 noiseEast, noiseNorth;
uniform float mapExtent;          // m
uniform float detailScale;        // detail noise period / shape noise period

uniform sampler2D weatherMap;  // r top cover, g middle cover, b towering, a shear (0.5 none)
uniform sampler2D colorMap;    // rgb albedo (display), a walls
uniform sampler3D shapeNoise;
uniform sampler3D detailNoise;
// The shape noise's fixed-depth reads as 2D (cloud_volume.c): r, g: shape r
// at z 0.37, 0.61; b, a: shape g at z 0.5, 0.81. And the walls' wander:
// r = shape r, g = shape r at p.yzx + 0.37.
uniform sampler2D shapeSlices;
uniform sampler3D wanderNoise;
uniform sampler2D plumeMap;    // anvil plumes (cloudplume.frag): r strength, g how far downwind

float remap(float v, float lo, float hi, float nlo, float nhi)
{
	return nlo + (v - lo) / (hi - lo) * (nhi - nlo);
}

// Cloud extinction (1/m) at rel (body axes, m from the map's centre on the
// reference sphere), altitude a; foot: a pixel there (m), for the noise
// mips; detail: with the edge erosion. alb: the albedo there (linear).
// base: the extinction without the erosion (what detail = false returns),
// so a march can test for cloud and erode it in one call.
//
// It reads as little as it can where there is no cloud: nothing between
// the layers, and no dome cells above the highest top a dome could have
// (unless a sheet, streak or the carpet is there). Each shortcut returns
// exactly what the full path would.
float densityCore(vec3 rel, float a, float foot, bool detail, out vec3 alb, out float base)
{
	alb = vec3(0.0);
	base = 0.0;
	if (a <= shellLo || a >= shellHi)
		return 0.0;
	bool inLayer = false;
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i >= layerCount)
			break;
		float hf = (a - layerBase[i]) / (layerTop[i] - layerBase[i]);
		if (hf > 0.0 && hf < 1.0)
			inLayer = true;
	}
	if (!inLayer)
		return 0.0;
	// xy: in the map's frame, for the map; the noise is read in its own.
	vec2 uv = vec2(dot(rel, mapEast), dot(rel, mapNorth)) / mapExtent + 0.5;
	vec3 reln = rel + noiseShift;
	vec2 xy = vec2(dot(reln, noiseEast), dot(reln, noiseNorth));
	float edge = smoothstep(0.0, 0.12, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	if (edge <= 0.0)
		return 0.0;
	// The walls wander with height: the map is read up to 0.8 of the
	// lowest layer's shape period away, the offset turning over ~3 periods
	// of height and ~10 across.
	float ws = layerScale[layerCount - 1];
	vec3 wq = vec3(xy / (ws * 10.0), a / (ws * 3.0));
	vec2 wv = textureLod(wanderNoise, wq, 0.0).rg - 0.5;
	vec2 uvw = uv + wv * (1.6 * ws / mapExtent);
	vec4 w = textureLod(weatherMap, uvw, 0.0);
	vec4 cw = textureLod(colorMap, uvw, 0.0);
	float shear = w.a * 2.0 - 1.0;
	vec3 mapAlb = displayToLinear(cw.rgb);
	// Anvil plume (one layer has them): a flat sheet near the layer's top
	// where a storm cell's top was carried downwind, sinking a little and
	// thinning as it goes; out over clear air too (no cover needed). Thin
	// (semi-transparent, so the erosion carves it) and fibrous: noise long
	// along the wind, fine across it. Worked out here, once, so the layer
	// loop carries a single value (a bigger loop costs the whole march).
	float plume = 0.0;
	if (a > plumeLo && a < plumeHi) {
		vec2 pl = textureLod(plumeMap, uvw, 0.0).rg;
		if (pl.r > 0.0) {
			float hfP = (a - layerBase[plumeLayer]) / (layerTop[plumeLayer] - layerBase[plumeLayer]);
			float pc = 0.88 - 0.1 * pl.g;             // its middle (layer heights)
			float pt = mix(0.07, 0.035, pl.g);        // half its thickness
			float slab = (1.0 - smoothstep(pc + 0.6 * pt, pc + pt, hfP))
			           * smoothstep(pc - 1.8 * pt, pc - 0.3 * pt, hfP);
			plume = 0.35 * layerPlume[plumeLayer] * pl.r * (1.0 - 0.5 * pl.g) * slab;
			if (plume > 0.0) {
				float scP = layerScale[plumeLayer];
				vec3 fq = vec3(xy.x / (scP * 5.0), xy.y / (scP * 0.6), hfP * 4.0) + vec3(0.13, 0.57, 0.31);
				float fib = textureLod(shapeNoise, fq, log2(max(foot / (scP * 0.6 / 128.0), 1.0))).r;
				plume *= 1.6 * smoothstep(0.35, 0.8, fib);
			}
		}
	}

	float total = 0.0;
	vec3 albSum = vec3(0.0);
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i >= layerCount)
			break;
		float hf = (a - layerBase[i]) / (layerTop[i] - layerBase[i]);
		if (hf <= 0.0 || hf >= 1.0)
			continue;
		float plumeI = i == plumeLayer ? plume : 0.0;
		float sc = layerScale[i];
		// Broad regions of thick cloud and wide valleys (~5 shape periods,
		// swept east-west), so the shape noise's tiling never shows.
		vec2 big = vec2(xy.x / (layerSweep[i] * 1.6), xy.y) / (sc * 5.3) + vec2(0.31, 0.77) * float(i);
		float broad = textureLod(shapeSlices, big, 0.0).r;
		float cover = clamp((i == 0 ? w.r : w.g) * layerCoverage[i] + (broad - 0.67) * 1.6, 0.0, 1.0);
		cover = max(cover, cw.a);
		// The carpet: low billows over the layer's lowest part, in patches
		// (thicker and higher in some, thin in others) a few shape periods
		// across, swept with the wind.
		float carpet = 0.0, mapCover = cover;
		if (layerCarpet[i] > 0.0) {
			vec2 cq = vec2(xy.x / (layerSweep[i] * 2.0), xy.y) / (sc * 3.0) + vec2(0.17, 0.41);
			float pat = smoothstep(0.3, 0.75, textureLod(shapeSlices, cq, 0.0).g);
			// Flat-topped: its top varies little, and ends over a short span.
			float cTop = 0.25 + 0.06 * pat + 0.08 * (broad - 0.67);
			carpet = 1.0 - smoothstep(cTop - 0.06, cTop, hf);
			cover = max(cover, layerCarpet[i] * mix(0.6, 1.0, pat) * carpet);
		}
		if (cover <= 0.01 && plumeI <= 0.0)
			continue;
		// Lean: the shapes slide downwind with height, more (either way)
		// where the wind shears.
		float lean = layerShear[i] * (0.5 + 3.0 * shear);
		vec3 np = vec3((xy.x + (a - layerBase[i]) * lean) / layerSweep[i], xy.y, a / layerStretch[i]) / sc
		        + float(i) * vec3(0.37, 0.71, 0.13);
		float lodS = log2(max(foot / (sc / 128.0), 1.0));
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
		// Above the highest top a dome could have (dome = 1 below), with
		// no carpet, sheet, streak or plume here: no cloud, whatever the cells.
		float topMax = max(clamp(mix(0.45, 1.0, w.b) * 1.1, 0.2, 1.0), cw.a);
		if (carpet <= 0.0 && hf >= topMax && sheet <= 0.0 && streak <= 0.0 && plumeI <= 0.0)
			continue;
		// Rounded tops, as high as the map's towering,
		// raised and lowered by broad cells (domes and valleys).
		// The cell noise falls off linearly from each cell's centre (a
		// cone); a circle profile makes it a dome. Walls go to the top.
		// Cells several times wider than the layer is tall, so the domes
		// are broad (narrow ones read as spires).
		float cell = 1.0 - mix(textureLod(shapeSlices, np.xy * 0.1, 0.0).b,
		                       textureLod(shapeSlices, np.yx * 0.043 + 0.13, 0.0).a, 0.45);
		float dome = sqrt(max(1.0 - 1.8 * cell * cell, 0.0));
		float topF = clamp(mix(0.45, 1.0, w.b) * (0.5 + 0.6 * dome), 0.2, 1.0);
		topF = max(topF, cw.a * (0.85 + 0.15 * dome));
		float hc = hf / topF;   // up the cloud, 0 at the base, 1 at its top
		// Anvil: where the wind shears, the top spreads and flattens and
		// the trunk below narrows; with the lean, the top overhangs.
		float anv = layerAnvil[i] * abs(shear);
		// (Clear air, here only for a plume, gets none.)
		float cov = cover > 0.01 ? clamp(cover + anv * (0.4 * smoothstep(0.55, 0.9, hc) - 0.15 * (1.0 - hc)), 0.0, 1.0) : 0.0;
		// Thinning toward the top: masses round off and their sides slope.
		cov *= 1.0 - 0.45 * smoothstep(0.35, 1.0, hc);
		float prof = 1.0 - smoothstep(topF * mix(0.55, 0.9, anv), topF, hf);
		prof = max(prof, carpet);
		if (prof <= 0.0 && sheet <= 0.0 && streak <= 0.0 && plumeI <= 0.0)
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
		// The plume, ragged with the shape noise (the erosion then sweeps
		// its edges into streaks along the wind).
		d = max(d, plumeI * mix(0.55, 1.1, shape));
		if (d <= 0.0)
			continue;
		float dBase = d * layerDensity[i] * edge;
		if (dBase > 0.0)
			base += dBase;
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

float density(vec3 rel, float a, float foot, bool detail, out vec3 alb)
{
	float base;
	return densityCore(rel, a, foot, detail, alb, base);
}
