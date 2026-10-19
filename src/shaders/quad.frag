#version 410

in vec2 fTexCoords;
in vec3 fragPos;
in vec3 fE;
in vec3 fN;
in vec3 fL;
in vec3 fH;
in vec3 vColor;
in vec4 fLightSpace;
in vec3 fSurfDir;
in mat3 fNormalMat;

out vec4 FragColor;

uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
uniform sampler2D depthMap;

// 3D simplex noise (Ashima Arts / I. McEwan, texture-free)
vec3 mod289v(vec3 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 mod289v(vec4 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289v(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v) {
	const vec2 C = vec2(1.0/6.0, 1.0/3.0);
	const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
	vec3 i  = floor(v + dot(v, C.yyy));
	vec3 x0 = v - i + dot(i, C.xxx);
	vec3 g = step(x0.yzx, x0.xyz);
	vec3 l = 1.0 - g;
	vec3 i1 = min(g.xyz, l.zxy);
	vec3 i2 = max(g.xyz, l.zxy);
	vec3 x1 = x0 - i1 + C.xxx;
	vec3 x2 = x0 - i2 + C.yyy;
	vec3 x3 = x0 - D.yyy;
	i = mod289v(i);
	vec4 p = permute(permute(permute(
			i.z + vec4(0.0, i1.z, i2.z, 1.0))
			+ i.y + vec4(0.0, i1.y, i2.y, 1.0))
			+ i.x + vec4(0.0, i1.x, i2.x, 1.0));
	float n_ = 0.142857142857;
	vec3 ns = n_ * D.wyz - D.xzx;
	vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
	vec4 x_ = floor(j * ns.z);
	vec4 y_ = floor(j - 7.0 * x_);
	vec4 x = x_ * ns.x + ns.yyyy;
	vec4 y = y_ * ns.x + ns.yyyy;
	vec4 h = 1.0 - abs(x) - abs(y);
	vec4 b0 = vec4(x.xy, y.xy);
	vec4 b1 = vec4(x.zw, y.zw);
	vec4 s0 = floor(b0)*2.0 + 1.0;
	vec4 s1 = floor(b1)*2.0 + 1.0;
	vec4 sh = -step(h, vec4(0.0));
	vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
	vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;
	vec3 p0 = vec3(a0.xy, h.x);
	vec3 p1 = vec3(a0.zw, h.y);
	vec3 p2 = vec3(a1.xy, h.z);
	vec3 p3 = vec3(a1.zw, h.w);
	vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
	p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
	vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
	m = m*m;
	return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

// Terrain height field + lighting controls (tweak here)
#define NOISE_FREQ      2.6
#define NOISE_OCTAVES   4
#define SURF_EPS        0.030   // angular step for the surface gradient
#define NORMAL_STRENGTH 0.62    // bump relief amount

float fbm3(vec3 p) {
	float v = 0.0;
	float amp = 0.5;
	float frq = NOISE_FREQ;
	for (int i = 0; i < NOISE_OCTAVES; i++) {
		v += amp * snoise(p * frq);
		frq *= 2.0;
		amp *= 0.5;
	}
	return v;
}

vec3 heightRamp(float e) {
	vec3 deep   = vec3(0.02, 0.09, 0.20);
	vec3 shall  = vec3(0.05, 0.22, 0.38);
	vec3 sand   = vec3(0.72, 0.64, 0.40);
	vec3 grass  = vec3(0.16, 0.38, 0.15);
	vec3 ridge  = vec3(0.42, 0.38, 0.34);
	vec3 snow   = vec3(0.92, 0.93, 0.95);
	float sea   = 0.50;
	float beach = 0.56;
	float tree  = 0.70;
	float peak  = 0.85;
	if (e < sea)   return mix(deep,  shall, smoothstep(0.0,   sea,   e));
	if (e < beach) return mix(shall, sand,  smoothstep(sea,   beach, e));
	if (e < tree)  return mix(sand,  grass, smoothstep(beach, tree,  e));
	if (e < peak)  return mix(grass, ridge, smoothstep(tree,  peak,  e));
	return mix(ridge, snow, smoothstep(peak, 1.0, e));
}

/*
fLight.xyz = fLight.xyz * 0.5 + 0.5;
float closestDepth = texture(depthMap, fLight.xy).r;
float currentDepth = fLight.z;
float shadow = 0.0;
vec2 texelSize = 1.0 / textureSize(depthMap, 0);
for(int x = -1; x <= 1; ++x) {
	for(int y = -1; y <= 1; ++y) {
		float pcfDepth = texture(depthMap, fLight.xy + vec2(x, y) * texelSize).r;
		shadow += currentDepth - 0.0001 > pcfDepth ? 1.0 : 0.0;
	}
}
shadow /= 9.0;

if(fLight.z > 1.0)
	shadow = 0.0;
return shadow;
*/

float shadowCalculation(vec4 fLight) {
	vec3 fragToLight = fragPos - lightPosition;
	//fragToLight = fLight.xyz * 0.5 + 0.5;
	//fragToLight = fragToLight/700.0;
	float closestDepth = texture(depthMap, vec2(fLight.xyz * 0.5 + 0.5)).r;
	float currentDepth = length(fragToLight)/7000.0;
	//closestDepth *= 700.0;

	float shadow = 0.0;
	vec2 texelSize = 1.0 / textureSize(depthMap, 0);
	for(int x = -1; x <= 1; ++x) {
		for(int y = -1; y <= 1; ++y) {
			float pcfDepth = texture(depthMap, vec2(fLight.xyz * 0.5 + 0.5) + vec2(x, y) * texelSize).r;
			shadow += currentDepth - 0.02 > pcfDepth ? 1.0 : 0.0;
		}
	}
	shadow /= 9.0;


	//shadow = currentDepth - 0.02 > closestDepth ? 1.0 : 0.0;
	//if(fragToLight.z > 1.0)
	//	shadow = 0.0;

	return shadow;
}

void main()
{
	vec3 ambientSky = vec3(0.10, 0.12, 0.16);   // cool skylight: keeps the night side off black
	const float PI = 3.14159265359;
	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;

	// 3D noise height field in the planet-local frame (stuck to the surface,
	// rotates with the planet). Every face samples this one continuous field,
	// so it is seamless across all six quadtree faces.
	vec3 dObj = normalize(fSurfDir);
	vec3 up = (abs(dObj.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 T = normalize(cross(up, dObj));
	vec3 B = cross(dObj, T);

	float h  = fbm3(dObj);
	float hT = fbm3(normalize(dObj + T * SURF_EPS));
	float hB = fbm3(normalize(dObj + B * SURF_EPS));

	float e = clamp(h * 0.5 + 0.5, 0.0, 1.0);
	vec4 color = vec4(heightRamp(e), 1.0);
	vec3 albedo = color.rgb;

	// Height-driven microfacet material: ocean = smooth-ish/low-F0, land = rough,
	// snow = in between (same bands as heightRamp: sea 0.50, beach 0.56, peak 0.85)
	float oceanMask = 1.0 - smoothstep(0.50, 0.56, e);
	float snowMask  = smoothstep(0.85, 0.95, e);
	float rough = mix(0.72, 0.15, oceanMask);   // ocean is no longer a perfect mirror
	rough = mix(rough, 0.45, snowMask);
	float a2 = max(rough * rough, 0.0004);
	vec3 F0 = mix(vec3(0.04), vec3(0.02), oceanMask);
	F0 = mix(F0, vec3(0.03), snowMask);

	// Bump relief scaled by material: a smooth ocean keeps a coherent sun glint
	// instead of scattering it into per-texel glitter; rough land keeps full relief.
	float bump = mix(NORMAL_STRENGTH, NORMAL_STRENGTH * 0.25, oceanMask);
	vec3 grad = T * ((hT - h) / SURF_EPS) + B * ((hB - h) / SURF_EPS);
	vec3 Nobj = normalize(dObj - bump * grad);
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

	vec3 lit = ambientSky * albedo;             // indirect sky/airlight term
	float wrap = 0.25;                           // wrap diffuse: soft terminator
	if (NdotL > -wrap) {
		float wl = clamp((NdotL + wrap) / (1.0 + wrap), 0.0, 1.0);
		vec3 kd = 1.0 - F;                       // energy conservation (dielectric)
		vec3 diff = albedo / PI * kd * wl;
		vec3 spec = vec3(0.0);
		if (NdotL > 0.0) {
			// Smith-Schlick geometry term. Unlike the Charlie visibility
			// approximation, G -> 0 as NdotL -> 0, so the specular cleanly vanishes
			// at grazing incidence instead of diverging to white. The NdotL
			// foreshortening is folded into the denominator (f·NdotL = D·G·F/(4·NdotV)),
			// so the lobe also fades correctly toward the terminator.
			float k = a2 * 0.5;
			float G1L = NdotL / (NdotL * (1.0 - k) + k);
			float G1V = NdotV / (NdotV * (1.0 - k) + k);
			spec = (D * G1L * G1V * F) / (4.0 * NdotV + 1e-4);
		}
		lit += (diff + spec) * sunRadiance;
	}

	// Filmic (ACES) tone map: compresses hot specular while preserving albedo
	// saturation, unlike Reinhard which trends everything toward white.
	float exposure = 1.05;
	vec3 x = lit * exposure;
	lit = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	float gamma = 2.2;
	FragColor = vec4(pow(lit, vec3(1.0 / gamma)), color.a);
}
