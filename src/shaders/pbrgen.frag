#version 410

in vec3 fSurfDir;

uniform int mapPass;

out vec4 FragColor;

// --- Exact copy of the planet's noise field (quad.frag) ---
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

// Same constants as the planet shader so the maps match what the planet renders
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

void main()
{
	// Identical surface frame + sampling as quad.frag
	vec3 dObj = normalize(fSurfDir);
	vec3 up = (abs(dObj.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 T = normalize(cross(up, dObj));
	vec3 B = cross(dObj, T);

	float h  = fbm3(dObj);
	float hT = fbm3(normalize(dObj + T * SURF_EPS));
	float hB = fbm3(normalize(dObj + B * SURF_EPS));
	float dhT = (hT - h) / SURF_EPS;
	float dhB = (hB - h) / SURF_EPS;
	float e = clamp(h * 0.5 + 0.5, 0.0, 1.0);

	if (mapPass == 0) {
		// Albedo: the terrain height ramp with subtle high-frequency detail
		vec3 alb = heightRamp(e);
		float detail = fbm3(dObj * 2.3 + vec3(5.2, 1.7, 9.4)) * 0.5 + 0.5;
		alb *= 0.85 + 0.30 * detail;
		FragColor = vec4(clamp(alb, 0.0, 1.0), 1.0);
	} else if (mapPass == 1) {
		// Height: the raw noise elevation field (0..1)
		FragColor = vec4(vec3(e), 1.0);
	} else if (mapPass == 2) {
		// Normal: tangent-space bump normal derived from the height gradient,
		// matching quad.frag's Nobj = normalize(dObj - NORMAL_STRENGTH * grad)
		vec3 n = normalize(vec3(-dhT * NORMAL_STRENGTH, -dhB * NORMAL_STRENGTH, 1.0));
		FragColor = vec4(n * 0.5 + 0.5, 1.0);
	} else if (mapPass == 3) {
		// Roughness: highest at the tallest peaks, falling off to ocean level
		// (e on this noise field spans ~0.2..0.82)
		float rough = mix(0.05, 0.92, smoothstep(0.50, 0.78, e));
		rough += 0.05 * (fbm3(dObj * 3.7 + vec3(9.1, 4.2, 7.7)) * 0.5 + 0.5);
		FragColor = vec4(vec3(clamp(rough, 0.0, 1.0)), 1.0);
	} else if (mapPass == 4) {
		// Metallic: random speckle confined to the high peaks (exposed rock)
		float m = smoothstep(0.66, 0.78, e)
		    * smoothstep(0.40, 0.62, fbm3(dObj * 1.8 + vec3(31.4, 3.9, 12.6)));
		FragColor = vec4(vec3(m * 0.9), 1.0);
	} else {
		// AO: occluded in the mountains and in random forested lowland patches
		float mountains = smoothstep(0.68, 0.80, e);
		float forest = smoothstep(0.54, 0.60, e)
		             * (1.0 - smoothstep(0.64, 0.70, e))
		             * smoothstep(0.45, 0.62, fbm3(dObj * 1.6 + vec3(13.7, 6.1, 22.3)));
		float ao = clamp(1.0 - 0.55 * mountains - 0.50 * forest, 0.05, 1.0);
		FragColor = vec4(vec3(ao), 1.0);
	}
}
