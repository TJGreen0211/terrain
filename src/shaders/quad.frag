#version 410
#extension GL_ARB_conservative_depth : enable

// Two programs are built from this source (Main.c), for a depth prepass:
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
#include "detail.glsl"

out vec4 FragColor;

uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
// Shadow map (Main.c initDepthbuffer): hardware depth compare, rendered
// from the sun's position with lightSpace = projection * view applied to
// camera-relative positions. shadowTexel is one texel's world size at the
// planet, used to scale the receiver offsets.
uniform sampler2DShadow depthMap;
uniform mat4 lightSpace;
uniform float shadowTexel;
// Camera-relative rendering: fragPos, lightPosition and bodyCenter (the
// planet centre) are all relative to the camera, which sits at the origin
// (cameraPos = lodCameraPos = 0). Planet-local positions are fragPos -
// bodyCenter.
uniform vec3 bodyCenter;
// Height field baked once at startup from the original fbm3 (heightbake.frag).
// lodK folds the pixel/texel angular-size ratio into log2(dist*lodK), plus
// the equirect pole and grazing corrections, clamped to the bake's 11 mips
// (2048 wide).
uniform sampler2D heightMap;
uniform vec3 lodCameraPos;
uniform float lodK;
// Sea level from the baked + eroded height field (TerrainGen.c); the height
// ramp and the material bands all key off this single value, and water.frag
// gets the identical value so the discard edge sits on the coastline.
uniform float seaLevel;
// Water column under sea level (same values and profile as water.frag), for
// skipping seafloor the water hides (see the deep-sea early-out in main).
uniform float oceanDepth;
uniform float oceanRelief;
const float WATER_ABSORB = 500.0;   // must match water.frag
// Main-pass near plane, to rewrite gl_FragDepth (see seafloorDepth).
// The main projection is infinite reversed-Z: depth = depthNear / distance
// along the view axis.
uniform float depthNear;

const float PI = 3.14159265359;

// Atmospheric extinction (OpenGLSolar planet.frag port): dims the surface by
// the transmittance along the surface-to-camera path through the dome's shell.
// The atmosphere dome pass adds the in-scattered light over both the limb and
// the planet disc; adding it here as well would count that radiance twice.
// applyAtmosphere takes planet-local eye/fragment positions (shell-centred).
uniform float hasAtmosphere;
uniform float fInnerRadius;
uniform float fOuterRadius;
uniform vec3 C_R;

// Keep the miss sentinel beyond the largest supported shell radius.
const float ATMO_MAX = 1e30;
uniform float K_R; // Rayleigh / Mie constants, shared with atmosphere.frag (Atmosphere.c)
uniform float K_M;
const int NUM_ATMO_STEPS = 5;
// atmoOptic straight up from fInnerRadius to fOuterRadius. The shell
// thickness cancels (scaleH = 4/thickness, scaleL = 1/thickness), so it is
// (1/N) * sum_i exp(-4 (i + 0.5) / N) for any radii: a constant, folded at
// compile time instead of a 5-step march per pixel. Keep in step with
// NUM_ATMO_STEPS and the 4.0 in scaleH.
const float TAU_ZENITH = 0.2 * (exp(-0.4) + exp(-1.2) + exp(-2.0) + exp(-2.8) + exp(-3.6));

vec2 shellIntersection(vec3 p, vec3 dir, float radius)
{
	float b = dot(p, dir);
	float c = dot(p, p) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(ATMO_MAX, -ATMO_MAX);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float atmoOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(NUM_ATMO_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < NUM_ATMO_STEPS; i++) {
		// Clamped at the ground: a sun ray that cuts through the planet
		// sees sea-level density along its chord instead of an exploding one.
		sum += exp(-max(length(v) - fInnerRadius, 0.0) * scaleH);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphere(vec3 color, vec3 eye, vec3 frag)
{
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	// Ray in the shell-centred frame: eye = camera, fragment at distance tFrag.
	vec3 ray = frag - eye;
	float tFrag = length(ray);
	vec3 dir = ray / max(tFrag, 1e-6);

	vec2 e = shellIntersection(eye, dir, fOuterRadius);
	if (e.x > e.y) {
		return color; // view ray misses the shell entirely
	}
	vec2 f = shellIntersection(eye, dir, fInnerRadius);
	// Clip the path at the shell exit, the inner sphere, and the actual
	// fragment hit: the tFrag term handles displaced surfaces, where f.x
	// would overshoot or fall short of the visible surface.
	// camera inside the shell: the path starts at the camera
	e.x = max(e.x, 0.0);
	e.y = min(e.y, tFrag);
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if (e.y <= e.x) {
		return color;
	}

	float tau = atmoOptic(eye + dir * e.x, eye + dir * e.y, scaleH, scaleL) * (4.0 * PI);
	return color * exp(-(K_R * C_R + K_M) * tau);
}

// Sunlight colour at p after the sun-ward path through the shell, relative to
// an overhead sun (so the midday tuning of sunRadiance is unchanged): white at
// noon, warming and dimming toward the terminator. SUN_EXTINCTION scales the
// excess optical depth (1 = same air as the sky dome, which reads a little
// orange across the whole day side). The horizon factor covers the sun's disc
// setting behind the geometric limb, sized by its angular radius.
#define SUN_EXTINCTION 0.5
vec3 sunTransmittance(vec3 p, vec3 l, float sunAngle)
{
	vec3 up = normalize(p);
	float horizon = smoothstep(-sunAngle, sunAngle, dot(up, l));
	if (hasAtmosphere < 0.5 || horizon <= 0.0)
		return vec3(horizon);
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	// Start on or above the ground: the shell geometry and displaced
	// fragments can sit slightly under fInnerRadius.
	p = up * max(length(p), fInnerRadius);
	float tSun = shellIntersection(p, l, fOuterRadius).y;
	float tau = atmoOptic(p, p + l * max(tSun, 0.0), scaleH, scaleL);
	float excess = max(tau - TAU_ZENITH, 0.0) * (4.0 * PI) * SUN_EXTINCTION;
	return exp(-(K_R * C_R + K_M) * excess) * horizon;
}

// Skylight on a surface as a fraction of its daytime strength: the sky
// itself is lit by the sun, so it fades through twilight (sun up to ~12 deg
// below the horizon) instead of lighting the night side. NIGHT_FLOOR is the
// starlight/airglow left over, so the dark side reads as dark, not black.
#define NIGHT_FLOOR vec3(0.003, 0.004, 0.006)
float skyFraction(float sunUp)
{
	return smoothstep(-0.2, 0.3, sunUp);
}

// Terrain height field + lighting controls (tweak here)
#define TEXEL_ANG       (2.0*PI/2048.0) // heightmap texel angle at the equator, mip 0
#define NORMAL_STRENGTH 0.62    // bump relief amount
#define SEA_CLIP        0.0005  // min. seafloor clearance under the water, planet radii
#define BEACH_OFF       0.012   // shoreline width above seaLevel
#define WET_SAND        0.003   // darker wet sand just above the waterline
#define LAKE_MIN        0.03    // climate A (lake depth) where lakes start
#define VALLEY_LOD      3.0     // extra mips for the river-valley greening
#define LAKE_LOD        2.0     // lakes must survive this many mips (no pit specks)
#define COAST_LOD       4.0     // beaches need open sea within ~2^COAST_LOD texels

// Climate map baked with the height field (TerrainGen.c): R moisture
// (wind-carried rain + rain shadow, stored at half scale for headroom),
// G river strength, B alluvial deposits, A lake depth. The style uniforms
// (palette, temperature/humidity response) come from g_planetStyle and are
// free to change for alien worlds.
uniform sampler2D climateMap;
uniform float landRelief;
uniform vec3 biomeColors[9];    // rows cold/temperate/hot x cols dry/medium/wet
uniform vec3 rockColor;
uniform vec3 snowColor;
uniform vec3 sandColor;
uniform vec3 saltColor;
uniform vec3 inlandWaterColor;
uniform float tempOffset;
uniform float lapseRate;
uniform float snowTemp;
uniform float humidity;
uniform float equatorLush;
uniform float hadleyStrength;
uniform float riverThreshold;
uniform float rockSlope;

// Equirect uv of a direction + the lod for that latitude (longitude texels
// shrink by cos(lat), so the same footprint needs a coarser mip).
vec2 mapUV(vec3 d, float lodBase, out float lod) {
	float z = clamp(d.z, -1.0, 1.0);
	float cosLat = max(sqrt(max(1.0 - z*z, 1e-4)), 0.02);
	lod = clamp(lodBase + log2(1.0/cosLat), 0.0, 11.0);
	return vec2((atan(d.y, d.x)/PI + 1.0)*0.5, asin(z)/PI + 0.5);
}

float heightAt(vec3 d, float lodBase) {
	float lod;
	vec2 uv = mapUV(d, lodBase, lod);
	return textureLod(heightMap, uv, lod).r;
}

// Seafloor albedo under the water (which adds its own tint): pale sand on
// the shelf, darker silt/rock toward the abyss.
// Linear albedo (sRGB ~(0.70, 0.66, 0.55) and ~(0.30, 0.33, 0.31)).
vec3 seafloorColor(float e) {
	vec3 sand = vec3(0.45, 0.39, 0.26);
	vec3 silt = vec3(0.07, 0.09, 0.08);
	return mix(silt, sand, smoothstep(seaLevel * 0.6, seaLevel, e));
}

// Cheap 3D value noise for colour variation (sampled on the unit sphere).
float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}
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

// Sun visibility from the shadow map: 1 = lit, 0 = fully shadowed.
// p is the camera-relative position, n the geometric normal and l the unit
// direction to the sun. The receiver is pushed out along the normal (more at
// grazing light, where acne starts) and toward the sun by about a texel, so
// it never tests against its own rasterized depth. 3x3 hardware-filtered
// taps one texel apart give a smooth ~4-texel penumbra.
float sunVisibility(vec3 p, vec3 n, vec3 l) {
	float ndl = clamp(dot(n, l), 0.0, 1.0);
	p += n * shadowTexel * (1.0 + 2.0 * (1.0 - ndl)) + l * shadowTexel;
	vec4 c = vec4(p, 1.0) * lightSpace;
	vec3 s = c.xyz / c.w * 0.5 + 0.5;
	if (s.z >= 1.0)
		return 1.0;
	vec2 texel = 1.0 / vec2(textureSize(depthMap, 0));
	float lit = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			lit += texture(depthMap, vec3(s.xy + vec2(x, y) * texel, s.z));
	return lit / 9.0;
}

// Ring shadow (Rings.c). Analytic rather than shadow-mapped: the ring is
// semi-transparent and banded, which a depth map cannot store. The sun ray
// from p crosses the ring plane at t; the transmittance there is
// exp(-tau / |mu|) with tau from the baked radial profile, looked up blurred
// by the sun's angular size over t (a physically sized penumbra).
uniform float hasRing;
uniform sampler1D ringProfile;
uniform vec3 ringCenter;     // camera-relative planet centre
uniform vec3 ringNormal;
uniform vec2 ringRadii;      // inner, outer (world units)
uniform float ringSunAngle;

// p relative to the ring centre, l the unit direction to the sun, maxT the
// distance to the sun.
float ringTransmittance(vec3 p, vec3 l, float maxT) {
	float mu = dot(l, ringNormal);
	if (hasRing < 0.5 || abs(mu) < 1e-4)
		return 1.0;
	float t = -dot(p, ringNormal) / mu;
	if (t <= 0.0 || t >= maxT)
		return 1.0;
	float r01 = (length(p + l * t) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	if (r01 <= 0.0 || r01 >= 1.0)
		return 1.0;
	float texelW = (ringRadii.y - ringRadii.x) / float(textureSize(ringProfile, 0));
	float tau = textureLod(ringProfile, r01, log2(max(t * ringSunAngle / texelW, 1.0))).a;
	return exp(-tau / abs(mu));
}

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
	if (e >= seaLevel)
		return gl_FragCoord.z;
	float R = length(fragPos - bodyCenter) / max(fRadius, 1e-4);
	float D = length(camDir);
	float clearance = max(SEA_CLIP * R, 4.0 * D / 8388608.0);
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
	vec3 up = (abs(dObj.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 T = normalize(cross(up, dObj));
	vec3 B = cross(dObj, T);

	// lod base: pixel angular footprint on the sphere (dist*thetaPixel/r,
	// folded into lodK) plus grazing correction (footprint grows by 1/cos of
	// the view angle at the surface).
	vec3 camDir = lodCameraPos - fragPos;
	float graz = max(abs(dot(normalize(fragPos - bodyCenter), normalize(camDir))), 0.15);
	float lodBase = log2(max(length(camDir)*lodK, 1e-4)) - log2(graz);
	// Gradient step = one texel of the mip being sampled, so the bump
	// resolves exactly the detail the lod shows: a fixed step was ~10
	// texels wide and blurred every crease up close.
	float eps = TEXEL_ANG * exp2(clamp(lodBase, 0.0, 11.0));
	float h  = heightAt(dObj, lodBase);
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
		float transmit = exp(-WATER_ABSORB * oceanRelief * dT * dT / graz);
		if (transmit < 0.01) {
			FragColor = vec4(0.0, 0.0, 0.0, 1.0);
			return;
		}
	}
	float hT = heightAt(normalize(dObj + T * eps), lodBase)
	         - heightAt(normalize(dObj - T * eps), lodBase);
	float hB = heightAt(normalize(dObj + B * eps), lodBase)
	         - heightAt(normalize(dObj - B * eps), lodBase);

	float e = clamp(h, 0.0, 1.0);
	vec3 grad = (T * hT + B * hB) / (2.0 * eps);   // central differences

	// Terrain detail slope (body-local, m/m; land only): the share the
	// vertices carry plus the shorter octaves per pixel (down to ~2 px,
	// wider when grazing), with the geometry's envelope. Done before the
	// material code so little else is live while it runs.
	vec3 gDetail = vec3(0.0);
	if (e >= seaLevel) {
		float altD = (e - seaLevel) / max(1.0 - seaLevel, 1e-3);
		gDetail = fDetailSlope;
		float envD = detailEnvelope(landRelief * altD * detailRadius, altD);
		if (envD > 0.0) {
			vec3 qD = detailCubePoint(fPatchAddr, fPatchUV);
			float footM = length(camDir) * detailPix;
			gDetail += envD * detailSlope(fPatchAddr.x, qD,
				detailPixel(fPatchAddr, fPatchUV, length(qD), footM, footM / graz));
		}
	}

	vec4 color = vec4(1.0);
	vec3 albedo;
	float rough, waterMask;

	if (e < seaLevel) {
		// Seafloor, seen through the translucent water (its depth was
		// pushed behind the shell in the prepass: seafloorDepth).
		albedo = seafloorColor(e);
		rough = 0.9;
		waterMask = 0.0;
	} else {
		float lod;
		vec2 uv = mapUV(dObj, lodBase, lod);
		vec4 clim = textureLod(climateMap, uv, lod);
		vec4 climWide = textureLod(climateMap, uv, min(lod + VALLEY_LOD, 11.0));

		// Colour variation: continent-scale patches + finer mottling, faded
		// by the pixel footprint.
		float n1 = vnoiseLod(dObj, 40.0, eps);
		float n2 = vnoiseLod(dObj, 160.0, eps);
		float n3 = vnoiseLod(dObj, 640.0, eps);

		// Temperature: latitude, altitude lapse, planet offset, a little noise.
		float alt01 = (e - seaLevel) / max(1.0 - seaLevel, 1e-3);

		float cosLatS = sqrt(max(1.0 - dObj.z*dObj.z, 0.0));
		// Calibrated so sea-level ice starts near 72 deg and the equatorial
	// snowline sits near 90 % of the relief. The altitude term is curved
	// (alt^1.5): the eroded relief has broad high massifs, and a linear
	// lapse would snow over whole ranges instead of just their peaks.
	float temp = cosLatS * 1.2 - 0.2 - lapseRate * pow(alt01, 1.5) + tempOffset + (n1 - 0.5) * 0.08;

		// Moisture: baked wind rain x latitude rain belts, wetter on
		// alluvial plains and along big rivers/lakes.
		float absLat = degrees(asin(min(abs(dObj.z), 1.0)));
		float moist = 2.0 * clim.r * mix(1.0, hadleyFactor(absLat), hadleyStrength) * humidity;
		moist += 0.2 * clim.b;
		moist += 0.3 * smoothstep(riverThreshold - 0.15, riverThreshold + 0.05, climWide.g);
		moist += 0.3 * smoothstep(LAKE_MIN, LAKE_MIN * 3.0, climWide.a);
		moist = clamp(moist + (n2 - 0.5) * 0.12, 0.0, 1.0);

		float tBiome = clamp((temp - 0.2) / 0.7, 0.0, 1.0);
		albedo = biomeAt(tBiome, moist);
		albedo *= 0.84 + 0.32 * (0.5 * n1 + 0.3 * n2 + 0.2 * n3);
		rough = mix(0.62, 0.9, moist);   // desert smoother, forest canopy rough

		// Slope: rise/run of the displaced surface; cliffs show bare rock.
		float slope = length(grad * (landRelief / max(1.0 - seaLevel, 1e-3)) + gDetail);
		float steep = smoothstep(rockSlope, rockSlope * 1.6, slope);
		float coldness = 1.0 - smoothstep(snowTemp - 0.06, snowTemp + 0.06, temp);

		// Beach on gentle coasts only (steep coasts become cliffs): wet sand
		// at the waterline, dry above, grey shingle when cold.
		// The coarse mip averages in the nearby seafloor, so it only dips
		// below sea level near an actual coast: low inland plains stay
		// plains instead of turning into beaches.
		float hNear = textureLod(heightMap, uv, min(lod + COAST_LOD, 11.0)).r;
		float coastal = 1.0 - smoothstep(seaLevel, seaLevel + 0.01, hNear);
		float beach = (1.0 - smoothstep(seaLevel + BEACH_OFF * 0.5, seaLevel + BEACH_OFF, e)) * (1.0 - steep) * coastal;
		float wet = 1.0 - smoothstep(seaLevel, seaLevel + WET_SAND, e);
		vec3 sandC = mix(sandColor, rockColor * 1.2, coldness * 0.7) * mix(1.0, 0.55, wet);
		albedo = mix(albedo, sandC, beach);
		rough = mix(rough, mix(0.75, 0.3, wet), beach);

		vec3 rockC = rockColor * (0.8 + 0.4 * n3);
		albedo = mix(albedo, rockC, steep);
		rough = mix(rough, 0.7, steep);

		// Lakes (flooded basins) where the climate is wet enough; dry
		// basins become salt pans. Rivers from the drainage map.
		// min() with a coarser mip drops one-texel erosion pits: only basins
		// with real area stay flooded.
		float lakeA = min(clim.a, textureLod(climateMap, uv, min(lod + LAKE_LOD, 11.0)).a * 2.0);
		float lake = smoothstep(LAKE_MIN, LAKE_MIN * 2.0, lakeA);
		float wetBasin = smoothstep(0.15, 0.3, moist);
		albedo = mix(albedo, saltColor, lake * (1.0 - wetBasin));
		rough = mix(rough, 0.5, lake * (1.0 - wetBasin));
		float river = smoothstep(riverThreshold, riverThreshold + 0.10, clim.g) * (1.0 - steep * 0.5);
		waterMask = max(lake * wetBasin, river);
		albedo = mix(albedo, inlandWaterColor, waterMask);
		rough = mix(rough, 0.06, waterMask);

		// Snow/ice: cold enough and not too steep to hold it; freezes water.
		float snow = coldness * (1.0 - smoothstep(rockSlope * 1.2, rockSlope * 2.0, slope) * (1.0 - waterMask));
		albedo = mix(albedo, snowColor * (0.92 + 0.08 * n2), snow);
		rough = mix(rough, 0.4, snow);
		waterMask *= 1.0 - snow;
	}
	color.rgb = albedo;
	float a2 = max(rough * rough, 0.0004);
	vec3 F0 = mix(vec3(0.04), vec3(0.02), waterMask);

	// Bump relief scaled by material: water keeps a coherent sun glint
	// instead of scattering it into per-texel glitter; land keeps full relief.
	float bump = mix(NORMAL_STRENGTH, NORMAL_STRENGTH * 0.2, waterMask);
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
		float vis = mix(1.0, sunVisibility(fragPos, upGeo, L), shadowWeight);
		vis *= ringTransmittance(fragPos - ringCenter, L, length(lightPosition - fragPos));
		// Sunlight reddened by its long path through the air near the
		// terminator, and cut off as the sun sets behind the limb.
		vec3 sunCol = sunTransmittance(fragPos - bodyCenter, L, ringSunAngle);
		lit += (diff + spec) * sunRadiance * sunCol * vis;
	}

	// Surface extinction only (linear radiance, before the tone map); the
	// atmosphere dome owns all in-scattered radiance.
	if (hasAtmosphere > 0.5) {
		lit = applyAtmosphere(lit, lodCameraPos - bodyCenter, fragPos - bodyCenter);
	}

	// Filmic (ACES) tone map: compresses hot specular while preserving albedo
	// saturation, unlike Reinhard which trends everything toward white.
	float exposure = 1.05;
	vec3 x = lit * exposure;
	lit = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	float gamma = 2.2;
	FragColor = vec4(pow(lit, vec3(1.0 / gamma)), color.a);
}
