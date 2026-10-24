#version 410
#extension GL_ARB_conservative_depth : enable

// Two programs are built from this source (terrain_draw.c), for a depth prepass:
// DEPTH_PREPASS writes only depth (the surface, with seafloor pushed behind
// the water, seafloorDepth below) and returns before any shading; the
// colour pass then runs the full shader with depth test >= and no depth
// writes, so each pixel is shaded about once. Writing gl_FragDepth in the
// colour pass instead disables early-Z on some GPUs (Intel ignores the
// depth_less hint) and ran this shader for every hidden fragment.
#ifdef DEPTH_PREPASS
#ifdef GL_ARB_conservative_depth
layout(depth_less) out float gl_FragDepth;
#endif
#endif

in vec2 fTexCoords;
in vec3 fragPos;
in vec3 fE;
in vec3 fN;
in vec3 fL;
in vec3 fH;
in vec3 vColor;
in vec3 fSurfDir;
in float fRadius;
in mat3 fNormalMat;
// Terrain detail (quad.vert, detail.glsl): patch address + uv for the
// per-pixel octaves, and the vertices' share of the slope.
flat in ivec4 fPatchAddr;
in vec2 fPatchUV;
in vec3 fDetailSlope;
in float fDetailH;
in vec3 fShadowPos;      // surface point for shadow lookups (quad.vert)
#include "detail.glsl"

out vec4 FragColor;

uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
// Sun shadows: cascades + the global map (shadow.glsl, shadows.c).
#define SHADOW_TAPS 9
#include "world/shadows/shadow.glsl"
// Camera-relative rendering: fragPos, lightPosition and bodyCenter (the
// planet centre) are all relative to the camera, which sits at the origin
// (cameraPos = lodCameraPos = 0). Planet-local positions are fragPos -
// bodyCenter.
uniform vec3 bodyCenter;
// Height field baked at startup (terrain_gen.c, heightbake.frag), a cube map
// looked up by body-local direction (planetmap.glsl). lodK folds the
// pixel/texel angular-size ratio into log2(dist*lodK), plus the grazing and
// face-corner corrections, clamped to the map's mips.
uniform samplerCube heightMap;
#include "planetmap.glsl"
uniform vec3 lodCameraPos;
uniform float lodK;
// Sea level from the baked + eroded height field (terrain_gen.c); the height
// ramp and the material bands all key off this single value, and water.frag
// gets the identical value so the discard edge sits on the coastline.
uniform float seaLevel;
// Water column under sea level (same optics as water.frag, watercolumn.glsl),
// for skipping seafloor the water hides (see the deep-sea early-out in main)
// and tinting what it lets through.
uniform float oceanDepth;
uniform float oceanRelief;
#include "world/ocean/watercolumn.glsl"
// Main-pass near plane, to rewrite gl_FragDepth (see seafloorDepth).
// The main projection is infinite reversed-Z: depth = depthNear / distance
// along the view axis.
uniform float depthNear;
// Clearance (body radii) the pushed seafloor keeps under sea level: the
// water shell's largest wave excursion (water_draw.c Water_Excursion), so troughs
// never dip below it. It used to be a fixed 0.0005 radii, 3.2 km at Earth's
// size: coastal pixels pushed that far back fell behind skirt walls and
// neighbouring land patches, which then showed through the clear shallows
// as blocks and slats along every shore.
uniform float seaClearance;
// Plus the water shell's chord sag at distance D: (D * seaSagK)^2 body radii
// (water_draw.c Water_SagK). Its coarser triangles cut kilometres inside
// the sphere from orbit, and without this the deep floor's black (main)
// hid their middles: dark blotches over the open sea.
uniform float seaSagK;

const float PI = 3.14159265359;

// Extinction toward the camera, sunlight colour, skylight (atmosphere.c).
#include "world/atmosphere/atmosurface.glsl"

// Terrain height field + lighting controls (tweak here). The *_LOD offsets
// are in mips of the cube map: tuned on the old 2048-wide equirect map and
// raised by log2(old texel / face-centre texel) = 0.65 at TERRAIN_MAP_SIZE
// 1024, so they keep their footprint on the ground.
#define BEACH_OFF       0.012   // shoreline width above seaLevel
#define WET_SAND        0.003   // darker wet sand just above the waterline
#define LAKE_MIN        0.03    // climate A (lake depth) where lakes start
#define VALLEY_LOD      3.65    // extra mips for the river-valley greening
#define LAKE_LOD        2.65    // lakes must survive this many mips (no pit specks)
#define COAST_LOD       4.65    // beaches need open sea within ~2^COAST_LOD texels
#define SNOW_SMOOTH     0.7     // share of the per-pixel detail's slope snow hides
#define ASPECT_TEMP     0.1    // temperature per unit slope facing the equator (~1.1 km of snowline)

// Climate map baked with the height field (terrain_gen.c): R moisture
// (wind-carried rain + rain shadow, stored at half scale for headroom),
// G river strength, B alluvial deposits, A lake depth. The style uniforms
// (palette, temperature/humidity response) come from g_planetStyle and are
// free to change for alien worlds.
uniform samplerCube climateMap;
uniform samplerCube surfaceMap;   // RGB slope, A ambient occlusion (terrain_gen.h)
// Occlusion strength (debug panel): the skylight takes all of it, the sun
// TERRAIN_AO_SUN of it, a cavity term that reads as depth from orbit where
// the shadow maps are too coarse to.
uniform float terrainAO;
#define TERRAIN_AO_SUN 0.6
uniform float landRelief;
uniform vec3 biomeColors[9];    // rows cold/temperate/hot x cols dry/medium/wet
uniform vec3 rockColor;
uniform vec3 snowColor;
uniform vec3 sandColor;
uniform vec3 saltColor;
uniform vec3 inlandWaterColor;
uniform float tempOffset;
uniform float lapsePerM;   // temperature drop per metre of height
uniform float lapseDry;
uniform float snowTemp;
uniform float treeTemp;
uniform float humidity;
uniform float equatorLush;
uniform float hadleyStrength;
uniform float riverThreshold;
uniform float rockSlope;

// Swash on the beaches: the shore waves (shorewaves.glsl, as water.frag)
// running up the sand. The sea state from the baked wind map (unit 10).
uniform samplerCube oceanMap;
uniform float windMax;
uniform float planetRadiusM;
uniform float time;
#include "world/ocean/wavecube.glsl"
uniform float waveCascadeScales[WAVE_CASCADES];
#include "world/ocean/seastate.glsl"
#include "world/ocean/shorewaves.glsl"

float heightAt(vec3 d, float lod) {
	return planetMapFine(heightMap, d, lod, seaCoastBand());
}

// Seafloor albedo under the water (which adds its own tint): pale sand on
// the shelf, darker silt/rock toward the abyss.
// Linear albedo (sRGB ~(0.70, 0.66, 0.55) and ~(0.30, 0.33, 0.31)).
vec3 seafloorColor(float e) {
	vec3 sand = vec3(0.45, 0.39, 0.26);
	vec3 silt = vec3(0.07, 0.09, 0.08);
	return mix(silt, sand, smoothstep(seaLevel * 0.6, seaLevel, e));
}

// Cheap 3D value noise for colour variation (sampled on the unit sphere;
// hash13 from seastate.glsl).
float vnoise(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	f = f*f*(3.0 - 2.0*f);
	return mix(mix(mix(hash13(i),                  hash13(i + vec3(1,0,0)), f.x),
	               mix(hash13(i + vec3(0,1,0)),    hash13(i + vec3(1,1,0)), f.x), f.y),
	           mix(mix(hash13(i + vec3(0,0,1)),    hash13(i + vec3(1,0,1)), f.x),
	               mix(hash13(i + vec3(0,1,1)),    hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}
// Noise octave at frequency f (cycles per radian), faded to its mean once
// a pixel covers half a cycle, so it never aliases from orbit.
float vnoiseLod(vec3 d, float f, float pixAng) {
	float fade = 1.0 - smoothstep(0.25, 0.6, pixAng * f);
	return mix(0.5, vnoise(d * f), fade);
}

// Procedural ground textures (terrain_detail.c, groundtex.frag): tileable
// rock / ground / sand / snow layers for close-up surface detail. RG =
// height slope per tile, B = albedo factor, A = height. A tile is one cell
// of the detail lattice at some level (detail.glsl), so texture coordinates
// are a small integer cell + fraction: exact anywhere on an Earth-size body,
// continuous across patches. The tiles do not line up across the 12 cube
// edges (each face has its own s/t axes): a seam there, only up close.
// Two lattice levels are blended by distance so a texel stays near a
// pixel at every range (no visible tiling, no popping), and the whole
// layer fades out above GROUND_FADE metres per pixel.
uniform sampler2DArray groundTex;
uniform float groundTexStrength;   // debug panel, 0 = off
#define GROUND_TEX_SIZE 512.0
#define GROUND_RELIEF   0.04          // height range per tile width
#define GROUND_FADE     vec2(40.0, 300.0)   // m per pixel: full .. gone

// Layers weighted by w (rock, ground, sand, snow) at lattice level n.
// Returns the albedo factor; grad gets the slope in metres per cube unit
// along the face's s/t axes (detailSlope's input).
float groundLevel(int n, vec4 w, float footM, float qLen, out vec2 grad)
{
	ivec2 cell;
	vec2 f;
	detailLattice(fPatchAddr, fPatchUV, n, cell, f);
	vec2 uv = vec2(cell & ivec2(63)) + f;
	float tileM = exp2(1.0 - float(n)) * detailRadius / qLen;
	float lod = log2(max(footM * GROUND_TEX_SIZE / tileM, 1.0));
	vec4 acc = vec4(0.0);
	if (w.x > 0.01) acc += w.x * textureLod(groundTex, vec3(uv, 0.0), lod);
	if (w.y > 0.01) acc += w.y * textureLod(groundTex, vec3(uv, 1.0), lod);
	if (w.z > 0.01) acc += w.z * textureLod(groundTex, vec3(uv, 2.0), lod);
	if (w.w > 0.01) acc += w.w * textureLod(groundTex, vec3(uv, 3.0), lod);
	// Slope per tile width -> metres per cube unit (a tile is 2^(1-n) units).
	grad = acc.rg * (GROUND_RELIEF * tileM * exp2(float(n - 1)));
	return acc.b + (1.0 - (w.x + w.y + w.z + w.w));   // skipped weight stays neutral
}

// Whittaker-style biome grid: temperature (cold -> hot) x moisture
// (dry -> wet), smoothly blended between the 9 palette entries.
vec3 biomeAt(float t, float m) {
	float tc = clamp(t, 0.0, 1.0) * 2.0;
	float mc = clamp(m, 0.0, 1.0) * 2.0;
	int ti = int(min(floor(tc), 1.0));
	int mi = int(min(floor(mc), 1.0));
	float tf = smoothstep(0.0, 1.0, tc - float(ti));
	float mf = smoothstep(0.0, 1.0, mc - float(mi));
	vec3 lo = mix(biomeColors[ti*3 + mi],     biomeColors[ti*3 + mi + 1],     mf);
	vec3 hi = mix(biomeColors[(ti+1)*3 + mi], biomeColors[(ti+1)*3 + mi + 1], mf);
	return mix(lo, hi, tf);
}

// The share of a pixel's ground where smoothstep(lo, hi, slope) holds (three
// thresholds at once), for a resolved slope s plus unresolved ones of
// variance v (both axes together), taken as isotropic Gaussian: 3 rings x 4
// directions at the Rayleigh radius' sextiles, each ring turned 30 deg.
vec3 steepShare(float s, float v, vec3 lo, vec3 hi)
{
	if (v < 1e-6)
		return smoothstep(lo, hi, vec3(s));
	const float RING[3] = float[3](0.604, 1.177, 1.893);
	const vec4 COS[3] = vec4[3](vec4(1.0, 0.0, -1.0, 0.0), vec4(0.866, -0.5, -0.866, 0.5),
	                            vec4(0.5, -0.866, -0.5, 0.866));
	float sa = sqrt(0.5 * v);
	vec3 acc = vec3(0.0);
	for (int k = 0; k < 3; k++) {
		float r = sa * RING[k];
		vec4 m = sqrt(max(vec4(s * s + r * r) + (2.0 * s * r) * COS[k], 0.0));
		for (int j = 0; j < 4; j++)
			acc += smoothstep(lo, hi, vec3(m[j]));
	}
	return acc / 12.0;
}

// Latitude rain belts (Hadley/Ferrel cells): wet equatorial ITCZ, dry
// subtropical belt near 27 deg, wetter mid-latitude storm track, dry
// poles. Multiplies the baked wind-carried rain, so rain shadows, dry
// continental interiors and the regional anomaly still punch exceptions
// through the lush equator (and wet windward coasts survive in the
// subtropics). equatorLush scales the equatorial peak.
float hadleyFactor(float absLatDeg) {
	float itcz    = exp(-pow(absLatDeg / 14.0, 2.0));
	float subtrop = exp(-pow((absLatDeg - 27.0) / 9.0, 2.0));
	float midlat  = exp(-pow((absLatDeg - 55.0) / 12.0, 2.0));
	return 0.65 + 0.75 * equatorLush * itcz - 0.35 * subtrop + 0.2 * midlat;
}

#include "world/rings/ringshadow.glsl"
// Cloud shadows (clouds.c).
#include "world/clouds/cloudlayer.glsl"
#include "engine/gfx/tonemap.glsl"

// Depth of this fragment for the prepass. Seafloor (e < seaLevel), seen
// through the translucent water: coarse LOD triangles straddling the coast
// interpolate their land height across pixels the fine field calls sea, so
// the floor can sit at or above the water (jagged strips, or z-fighting the
// shell in the shallows). Discarding would punch a hole now that the water
// is see-through, so push the fragment's depth along the view ray to just
// under the shell instead: the water then draws over it and the coastline
// follows the per-pixel mask at every LOD. The clearance grows with the
// float reversed-Z depth resolution, a constant ~2^-23 of the distance, so
// it holds at range.
float seafloorDepth(float e, vec3 camDir, float graz)
{
#ifdef DEPTH_PREPASS
	// Static use of gl_SampleID makes the prepass run per MSAA sample with
	// gl_FragCoord.z at the sample (terrain_draw.c). glMinSampleShading
	// alone is not enough on AMD: it kept the pixel-centre z, so the colour
	// pass's >= test failed on sloped samples and the planet went
	// see-through. Never true; it only has to be there.
	if (gl_SampleID < 0)
		return 0.0;
#endif
	if (e >= seaLevel)
		return gl_FragCoord.z;
	float R = length(fragPos - bodyCenter) / max(fRadius, 1e-4);
	float D = length(camDir);
	float sag = D * seaSagK;
	float clearance = max((seaClearance + sag * sag) * R, 4.0 * D / 8388608.0);
	float poke = (fRadius - 1.0) * R + clearance;   // world units above the target
	if (poke <= 0.0)
		return gl_FragCoord.z;
	// Reversed-Z infinite: depth = near / zEye; view-space depth scales with
	// distance along the ray.
	float zEye = depthNear / max(gl_FragCoord.z, 1e-30);
	float zNew = zEye * (D + poke / graz) / max(D, 1e-4);
	return depthNear / zNew;
}

void main()
{
	vec3 ambientSky = vec3(0.10, 0.12, 0.16);   // cool skylight: keeps the night side off black
	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;

	// Height field sampled from the startup-baked texture (rotates with the
	// planet: dObj lives in the planet-local frame). Every face samples this
	// one continuous field, so it is seamless across all six quadtree faces.
	vec3 dObj = normalize(fSurfDir);

	// lod: pixel angular footprint on the sphere (dist*thetaPixel/r,
	// folded into lodK) plus grazing correction (footprint grows by 1/cos of
	// the view angle at the surface) plus the face-corner bias.
	vec3 camDir = lodCameraPos - fragPos;
	float graz = max(abs(dot(normalize(fragPos - bodyCenter), normalize(camDir))), 0.15);
	float lodBias = planetMapLodBias(dObj);
	float maxLod = planetMapMaxLod(heightMap);
	float lod = clamp(log2(max(length(camDir)*lodK, 1e-4)) - log2(graz) + lodBias, 0.0, maxLod);
	// Gradient step = one texel of the mip being sampled (its smallest
	// extent here), so the bump resolves exactly the detail the lod shows:
	// a fixed step was ~10 texels wide and blurred every crease up close.
	float eps = planetMapTexel(heightMap) * exp2(lod - lodBias);
	float h  = heightAt(dObj, lod);
#ifdef DEPTH_PREPASS
	gl_FragDepth = seafloorDepth(clamp(h, 0.0, 1.0), camDir, graz);
	return;
#endif
	// Deep sea: the water drawn over this pixel lets through at most its
	// column transmittance of the seafloor (water.frag blends with alpha >=
	// 1 - transmit, in display space), so below 1% the floor cannot show
	// by more than ~1/255 of its (dark) brightness: skip all its lighting.
	if (h < seaLevel) {
		float dT = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
		float transmit = waterTransmit(dT, oceanRelief, graz);
		if (transmit < 0.01) {
			FragColor = vec4(0.0, 0.0, 0.0, 1.0);
			return;
		}
	}
	// Slope (raw height per radian, body-local) and ambient occlusion, baked
	// with the field (terrain_gen.c surfacePixels) and read at the same mip:
	// one fetch instead of four height differences, and the slope filters
	// smoothly instead of showing the height texels.
	vec4 surf = textureLod(surfaceMap, dObj, lod);
	float e = clamp(h, 0.0, 1.0);
	vec3 grad = surf.xyz;
	float ao = surf.w;

	// Terrain detail slope (body-local, m/m; land only): the share the
	// vertices carry plus the shorter octaves per pixel (down to ~2 px,
	// wider when grazing), with the geometry's envelope. Done before the
	// material code so little else is live while it runs.
	// slopeVar: the variance of the finer slopes the pixel holds but does
	// not shade (detailUnresolvedVar), for the materials.
	vec3 gDetail = vec3(0.0);
	vec3 gPixel = vec3(0.0);   // the per-pixel octaves' share of gDetail
	float slopeVar = 0.0;
	if (e >= seaLevel) {
		float altD = (e - seaLevel) / max(1.0 - seaLevel, 1e-3);
		gDetail = fDetailSlope;
		vec3 qD = detailCubePoint(fPatchAddr, fPatchUV);
		DetailSpec specD = detailSpec(landRelief * altD * detailRadius, length(grad), altD, length(qD));
		if (specD.coast > 0.0) {
			float footM = length(camDir) * detailPix;
#if DETAIL_PIXEL_OCTAVES > 0
			gPixel = specD.coast * detailSlope(fPatchAddr.x, qD,
				detailPixel(fPatchAddr, fPatchUV, length(qD), footM, footM / graz, specD,
				            dot(fDetailSlope, fDetailSlope),
				            detailCubeGrad(fPatchAddr.x, qD, grad * detailSlopeToRise + fDetailSlope)));
			gDetail += gPixel;
#endif
			slopeVar = specD.coast * specD.coast
			         * detailUnresolvedVar(length(qD), footM, footM / graz, specD, dot(gDetail, gDetail));
		}
	}

	vec4 color = vec4(1.0);
	vec3 albedo;
	float rough, waterMask;

	if (e < seaLevel) {
		// Seafloor, seen through the translucent water (its depth was
		// pushed behind the shell in the prepass: seafloorDepth).
		// The water's alpha takes out the blue share of the column; the
		// red and green it absorbs faster are taken out here.
		float dT = clamp((seaLevel - e) / max(oceanDepth, 1e-4), 0.0, 1.0);
		albedo = seafloorColor(e) * waterFloorTint(dT, oceanRelief, graz);
		rough = 0.9;
		waterMask = 0.0;
	} else {
		vec4 clim = textureLod(climateMap, dObj, lod);
		vec4 climWide = textureLod(climateMap, dObj, min(lod + VALLEY_LOD, maxLod));

		// Colour variation: continent-scale patches + finer mottling, faded
		// by the pixel footprint.
		float n1 = vnoiseLod(dObj, 40.0, eps);
		float n2 = vnoiseLod(dObj, 160.0, eps);
		float n3 = vnoiseLod(dObj, 640.0, eps);

		// Moisture: baked wind rain x latitude rain belts, wetter on
		// alluvial plains and along big rivers/lakes.
		float absLat = degrees(asin(min(abs(dObj.z), 1.0)));
		// humidity scales every term: 0 = a dry world (no lakes, no rivers).
		float moist = 2.0 * clim.r * mix(1.0, hadleyFactor(absLat), hadleyStrength);
		moist += 0.2 * clim.b;
		moist += 0.3 * smoothstep(riverThreshold - 0.15, riverThreshold + 0.05, climWide.g);
		moist += 0.3 * smoothstep(LAKE_MIN, LAKE_MIN * 3.0, climWide.a);
		moist *= humidity;
		moist = clamp(moist + (n2 - 0.5) * 0.12, 0.0, 1.0);

		// Temperature: latitude, a lapse with height in metres, planet
		// offset, a little noise. Sea-level ice starts near 72 deg; the
		// snowline and treeline are in metres (lapsePerM, terrain_gen.c),
		// the same on any planet. The height is the map's per pixel (the
		// mesh's vertices read a coarser mip far away, which lowers narrow
		// summits by km) plus the vertices' detail, so the snow follows the
		// detail's peaks. Dry
		// ground lapses less (TERRAIN_LAPSE_DRY): a higher snowline there
		// (no snow cap on every high desert plateau); a dry world (humidity
		// 0) keeps the full lapse.
		float cosLatS = sqrt(max(1.0 - dObj.z*dObj.z, 0.0));
		float hM = max(landRelief * planetRadiusM * (e - seaLevel) / max(1.0 - seaLevel, 1e-3) + fDetailH, 0.0);
		float dry = (1.0 - moist) * min(humidity * 4.0, 1.0);
		// Aspect: slopes facing the equator are warmer, those facing the
		// pole colder (sun exposure), so the snow and treelines follow the
		// ridges instead of a contour.
		vec3 slopeV = grad * (landRelief / max(1.0 - seaLevel, 1e-3)) + gDetail;
		slopeV -= dObj * dot(slopeV, dObj);
		vec3 poleward = vec3(0.0, 0.0, sign(dObj.z)) - dObj * abs(dObj.z);
		float aspect = dot(slopeV, poleward) / max(length(poleward), 1e-4);
		float temp = cosLatS * 1.2 - 0.2 - lapsePerM * hM * (1.0 - lapseDry * dry)
		           + ASPECT_TEMP * clamp(aspect, -1.0, 1.0) + tempOffset + (n1 - 0.5) * 0.08;
		// Above the treeline (treeTemp): alpine meadow, then a fell field of
		// thin soil and gravel toward the snowline.
		float alpine = 1.0 - smoothstep(treeTemp - 0.03, treeTemp + 0.03, temp);
		float barren = (1.0 - smoothstep(snowTemp + 0.02, mix(snowTemp, treeTemp, 0.6), temp))
		             * smoothstep(0.3, 0.7, n3 + 0.3 * n2);

		float tBiome = clamp((temp - 0.2) / 0.7, 0.0, 1.0);
		// No forest above the treeline: the wet columns (taiga, forest) give
		// way to the medium one (meadow).
		albedo = biomeAt(tBiome, min(moist, mix(1.0, 0.5, alpine)));
		albedo *= 0.84 + 0.32 * (0.5 * n1 + 0.3 * n2 + 0.2 * n3);
		rough = mix(0.62, 0.9, moist);   // desert smoother, forest canopy rough

		// Slope: rise/run of the displaced surface; cliffs show bare rock,
		// and on alpine ground's thin soil on gentler slopes too. Scree
		// lies in the band just below the rock, at its angle of repose.
		// Shares of the pixel's ground, with its unresolved slopes.
		float slope = length(slopeV);
		float rockS = rockSlope * mix(1.0, 0.7, alpine);
		// x rock, y scree's band from below, z the snow sliding off.
		vec3 shares = steepShare(slope, slopeVar, vec3(rockS, rockS * 0.6, rockSlope),
		                         vec3(rockS * 1.6, rockS, rockSlope * 1.8));
		float steep = shares.x;
		float scree = max(shares.y - steep, 0.0) * mix(0.3, 0.9, alpine);
		float coldness = 1.0 - smoothstep(snowTemp - 0.06, snowTemp + 0.06, temp);

		// Beach on gentle coasts only (steep coasts become cliffs): wet sand
		// at the waterline, dry above, grey shingle when cold.
		// The coarse mip averages in the nearby seafloor, so it only dips
		// below sea level near an actual coast: low inland plains stay
		// plains instead of turning into beaches.
		float hNear = textureLod(heightMap, dObj, min(lod + COAST_LOD, maxLod)).r;
		float coastal = 1.0 - smoothstep(seaLevel, seaLevel + 0.01, hNear);
		float beach = (1.0 - smoothstep(seaLevel + BEACH_OFF * 0.5, seaLevel + BEACH_OFF, e)) * (1.0 - steep) * coastal;
		float wet = 1.0 - smoothstep(seaLevel, seaLevel + WET_SAND, e);
		vec3 sandC = mix(sandColor, rockColor * 1.2, coldness * 0.7) * mix(1.0, 0.55, wet);
		albedo = mix(albedo, sandC, beach);
		rough = mix(rough, mix(0.75, 0.3, wet), beach);

		// Swash (shorewaves.glsl): each wave's sheet of water running up
		// the beach and sliding back, the foam on its leading edge, and the
		// sand it wets. Exposure from the sea state just offshore (the
		// wind map is the sea's), as water.frag's surf.
		float sheet = 0.0, swashEdge = 0.0, swashWet = 0.0;
		float swashOn = coastal * (1.0 - steep);
		// Cheap reach test first, from the baked slope: the swash needs the
		// smooth height's own slope (two more height reads), only within
		// the wet sand's reach of the waterline.
		float inlandEst = (e - seaLevel) / max(length(grad - dObj * dot(grad, dObj)), 1e-6) * planetRadiusM;
		float reachMax = SHORE_RUNUP_M * SHORE_WET_REACH * shoreScale() * 1.8;
		if (oceanRelief > 0.0 && swashOn > 0.0 && e < seaLevel + BEACH_OFF && inlandEst < 2.0 * reachMax) {
			vec3 Tq = normalize(vec3(-dObj.y, dObj.x, 1e-4));
			vec3 Bq = normalize(cross(dObj, Tq));
			vec3 seaward;
			float inland = -shoreCoast(dObj, h, lod, Tq, Bq, seaward);
			vec4 seaS = textureLod(oceanMap, normalize(dObj + seaward * (2.0 * planetMapTexel(heightMap))), lod);
			float strength = mix(0.2, 1.0, smoothstep(0.1, 0.6, seaS.a) * smoothstep(2.0, 8.0, seaS.r * windMax))
			               * seaIceCalm(dObj) * swashOn;
			float footS = length(camDir) * lodK * planetMapTexel(heightMap) / graz * planetRadiusM;
			shoreSwash(max(inland, 0.0), dObj, strength, footS, sheet, swashEdge, swashWet);
			swashWet *= strength;
		}
		// Wet sand: darker and glossier.
		albedo *= mix(1.0, 0.6, swashWet);
		rough = mix(rough, 0.3, swashWet);

		vec3 rockC = rockColor * (0.8 + 0.4 * n3);
		// Fell field and scree: lighter, broken rock over the vegetation.
		albedo = mix(albedo, rockC * 1.15, max(barren * 0.75, scree));
		albedo = mix(albedo, rockC, steep);
		rough = mix(rough, 0.7, max(max(barren * 0.75, scree), steep));

		// Lakes (flooded basins) where the climate is wet enough; dry
		// basins become salt pans. Rivers from the drainage map.
		// min() with a coarser mip drops one-texel erosion pits: only basins
		// with real area stay flooded.
		float lakeA = min(clim.a, textureLod(climateMap, dObj, min(lod + LAKE_LOD, maxLod)).a * 2.0);
		// No basins flood (or dry to salt) on a world with no water at all.
		float lake = smoothstep(LAKE_MIN, LAKE_MIN * 2.0, lakeA) * min(humidity * 4.0, 1.0);
		float wetBasin = smoothstep(0.15, 0.3, moist);
		albedo = mix(albedo, saltColor, lake * (1.0 - wetBasin));
		rough = mix(rough, 0.5, lake * (1.0 - wetBasin));
		float river = smoothstep(riverThreshold, riverThreshold + 0.10, clim.g) * (1.0 - steep * 0.5);
		waterMask = max(lake * wetBasin, river);
		albedo = mix(albedo, inlandWaterColor, waterMask);
		rough = mix(rough, 0.06, waterMask);
		// The swash sheet: a film of water over the sand (its colour stays
		// the wet sand's), foam on its edge.
		rough = mix(rough, 0.06, sheet);
		waterMask = max(waterMask, sheet);
		albedo = mix(albedo, vec3(0.75, 0.78, 0.80), swashEdge);
		rough = mix(rough, 0.8, swashEdge);

		// Snow/ice: cold enough and not too steep to hold it; freezes water.
		float snow = coldness * (1.0 - shares.z * (1.0 - waterMask));
		albedo = mix(albedo, snowColor * (0.92 + 0.08 * n2), snow);
		rough = mix(rough, 0.4, snow);
		// Snow buries the small relief: the shading keeps less of the
		// per-pixel octaves under it (the materials above used them all).
		gDetail -= SNOW_SMOOTH * snow * gPixel;
		waterMask *= 1.0 - snow;

		// Ground textures near the ground, by material (not on water).
		float footG = length(camDir) * detailPix;
		float texW = groundTexStrength * (1.0 - smoothstep(GROUND_FADE.x, GROUND_FADE.y, footG))
		           * (1.0 - waterMask);
		if (texW > 0.0) {
			float wRock = max(steep, max(scree, barren * 0.75)) * (1.0 - snow);
			float wSand = beach * (1.0 - snow);
			vec4 w = vec4(wRock, max(1.0 - wRock - wSand - snow, 0.0), wSand, snow);
			vec3 qG = detailCubePoint(fPatchAddr, fPatchUV);
			float qLen = length(qG);
			// Level whose texels are one pixel wide; blend it with the next
			// coarser one (texels 1-2 and 2-4 px).
			float L = clamp(log2(2.0 * detailRadius / (GROUND_TEX_SIZE * footG * qLen)), 1.0, 30.0);
			int n0 = int(floor(L));
			float t = L - float(n0);
			vec2 g0, g1;
			float a0 = groundLevel(n0 - 1, w, footG, qLen, g0);
			float a1 = groundLevel(n0, w, footG, qLen, g1);
			gDetail += texW * detailSlope(fPatchAddr.x, qG, mix(g0, g1, t));
			albedo *= mix(1.0, mix(a0, a1, t), texW);
		}
	}
	color.rgb = albedo;
	float a2 = max(rough * rough, 0.0004);
	vec3 F0 = mix(vec3(0.04), vec3(0.02), waterMask);

	// Bump relief: the map's slope (height units per radian) as the true
	// rise / run, as the geometry has it (terrainlift.glsl: linear in the
	// land's height, quadratic in the sea floor's depth). Water keeps a
	// coherent sun glint instead of scattering it into per-texel glitter.
	float dTn = clamp((seaLevel - e) / max(oceanDepth, 1e-4), 0.0, 1.0);
	float rise = e >= seaLevel ? landRelief / max(1.0 - seaLevel, 1e-3)
	                           : 2.0 * oceanRelief * dTn / max(oceanDepth, 1e-4);
	float bump = mix(1.0, 0.2, waterMask) * rise;
	vec3 Nobj = normalize(dObj - bump * grad - gDetail);
	vec3 N = normalize(fNormalMat * Nobj);

	vec3 V = normalize(cameraPos - fragPos);
	vec3 L = normalize(fL);
	vec3 H = normalize(L + V + vec3(1e-5));     // guard the L == -V (backscatter) case
	float NdotL = dot(N, L);
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = max(dot(N, H), 0.0);
	float VdotH = max(dot(V, H), 0.0);

	// GGX (Trowbridge-Reitz) normal distribution
	float g = NdotH * NdotH * (a2 - 1.0) + 1.0;
	float D = a2 / (PI * g * g + 1e-7);
	// Schlick fresnel
	vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

	// Indirect sky/airlight term: follows the sun through twilight, and
	// slopes facing away from the zenith see less of the sky dome.
	vec3 upGeo = normalize(fragPos - bodyCenter);
	float sunUp = dot(upGeo, L);
	vec3 lit = (ambientSky * skyFraction(sunUp) * (0.6 + 0.4 * max(dot(N, upGeo), 0.0))
	            * mix(1.0, ao, terrainAO)
	          + NIGHT_FLOOR) * albedo;
	// Plain Lambert: the cosine falls to zero at the terminator on its own,
	// and sunTransmittance supplies the reddening. (A wrap term left ~20 %
	// of the light at the horizon for the sunset fade to cut off hard.)
	if (NdotL > 0.0) {
		vec3 kd = 1.0 - F;                       // energy conservation (dielectric)
		vec3 diff = albedo / PI * kd * NdotL;
		// Smith-Schlick geometry term. Unlike the Charlie visibility
		// approximation, G -> 0 as NdotL -> 0, so the specular cleanly vanishes
		// at grazing incidence instead of diverging to white. The NdotL
		// foreshortening is folded into the denominator (f·NdotL = D·G·F/(4·NdotV)),
		// so the lobe also fades correctly toward the terminator.
		float k = a2 * 0.5;
		float G1L = NdotL / (NdotL * (1.0 - k) + k);
		float G1V = NdotV / (NdotV * (1.0 - k) + k);
		vec3 spec = (D * G1L * G1V * F) / (4.0 * NdotV + 1e-4);
		// Cast shadows (terrain, moon). Below the geometric terminator the
		// planet body itself occludes the sun in the shadow map; fade the
		// test out there so sunlit peaks past the terminator are not cut
		// off by a hard self-shadow edge.
		float shadowWeight = smoothstep(-0.06, 0.0, sunUp);
		// Offset along the triangle's own normal (oriented outward), not
		// the sphere's up: on steep slopes up runs into the surface, and
		// the fine cascades resolve the difference. Near-vertical faces
		// (skirt walls glimpsed through patch-edge cracks, which lie under
		// the neighbouring surface) keep up, or they would show as shadowed
		// lines along every patch edge.
		vec3 faceN = normalize(cross(dFdx(fragPos), dFdy(fragPos)));
		float faceUp = dot(faceN, upGeo);
		faceN *= sign(faceUp + 1e-6);
		vec3 shadowN = normalize(mix(upGeo, faceN, smoothstep(0.25, 0.5, abs(faceUp))));
		float vis = mix(1.0, sunVisibility(fShadowPos, shadowN, L), shadowWeight);
		vis *= ringTransmittance(fragPos - ringCenter, L, length(lightPosition - fragPos));
		vis *= cloudShadow(fragPos - bodyCenter, L, length(camDir) * cloudPixAngle);
		// Sunlight reddened by its long path through the air near the
		// terminator, and cut off as the sun sets behind the limb.
		vec3 sunCol = sunTransmittance(fragPos - bodyCenter, L, ringSunAngle);
		lit += (diff * mix(1.0, ao, terrainAO * TERRAIN_AO_SUN) + spec) * sunRadiance * sunCol * vis;
	}

	// Debug panel: which shadow level serves each pixel.
	if (shadowDebug == 1)
		lit *= shadowDebugTint(fragPos);

	// Surface extinction only (linear radiance, before the tone map); the
	// atmosphere dome owns all in-scattered radiance.
	if (hasAtmosphere > 0.5) {
		lit = applyAtmosphere(lit, lodCameraPos - bodyCenter, fragPos - bodyCenter);
	}

	// Filmic (ACES) tone map: compresses hot specular while preserving albedo
	// saturation, unlike Reinhard which trends everything toward white.
	FragColor = vec4(toneMap(lit), color.a);
}
