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
	float currentDepth = length(fragToLight)/700.0;
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
	vec4 ambientProduct = vec4(0.20, 0.25, 0.34, 1.0);
	vec4 diffuseProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.8, 0.8, 0.8, 1.0);
	vec4 specularProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.5, 0.5, 0.5, 1.0);
	float shininess = 25.0;

	vec3 ray = normalize(fragPos - cameraPos);
	vec3 fH = -normalize((fragPos - lightPosition) + ray).xyz;

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
	vec3 grad = T * ((hT - h) / SURF_EPS) + B * ((hB - h) / SURF_EPS);
	vec3 Nobj = normalize(dObj - NORMAL_STRENGTH * grad);
	vec3 N = normalize(fNormalMat * Nobj);

	vec4 color = vec4(heightRamp(clamp(h * 0.5 + 0.5, 0.0, 1.0)), 1.0);

	float Kd = max(dot(fL, N), 0.0);
	float Ks = pow(max(dot(N, fH), 0.0), shininess);
	vec3 ambient = ambientProduct.xyz * color.rgb;
	vec3 diffuse = Kd * diffuseProduct.xyz * color.rgb;
	vec3 specular = Ks * specularProduct.xyz;
	if(dot(fL, N) < 0.0) {
		specular = vec3(0.0, 0.0, 0.0);
	}

	float gamma = 2.2;
	FragColor = vec4(pow(ambient+diffuse+specular, vec3(1.0/gamma)), color.a);
}
