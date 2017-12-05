#version 410

in vec4 fLightSpace;
in vec3 fTangent;
in vec3 fragPos;
in vec3 fPosition;
in vec3 fN;
in vec3 lightDir;
in vec3 vColor;
in vec2 fTexCoords;

out vec4 FragColor;

uniform mat4 model;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
uniform sampler2D depthMap;
uniform sampler2D noiseTexture;
uniform mat4 lightSpace;

const vec3 seaColor = vec3(0.1,0.19,0.22);
const vec3 seaBase = vec3(0.1,0.19,0.22);

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

	return shadow /= 9.0;
}

float getDiffuse(vec3 n, vec3 l, float p) {
    //return pow(dot(n,l) * 0.4 + 0.6, p);
	return max(dot(l, n), 0.0);
}

float getSpecular(vec3 n, vec3 h, float s) {
    //float nrm = (s + 8.0) / (3.1415 * 8.0);
    //return pow(max(dot(reflect(e,n),l),0.0),s) * nrm;

	return pow(max(dot(n, h), 0.0), s);
}

vec3 getSkyColor(vec3 e) {
    e.y = max(e.y,0.0);
    vec3 ret;
    ret.x = pow(1.0-e.y,2.0);
    ret.y = 1.0-e.y;
    ret.z = 0.6+(1.0-e.y)*0.4;
    return ret;
}

vec3 getSeaColor(vec3 point, vec3 normal, vec3 light, vec3 cam, vec3 dist, vec3 hf) {
	float fresnel = 1.0 - max(dot(normal, -cam), 0.0);
	fresnel = pow(fresnel, 3.0) * 0.65;

	vec3 reflected = getSkyColor(reflect(cam,normal));
	vec3 refracted = getDiffuse(normal, light, 80.0) * seaBase;
	float specular = getSpecular(normal, hf, 100.0);

	vec3 color = refracted;

	//float atten = max(1.0 - dot(dist,dist) * 0.001, 0.0);
    color += seaColor * (normal.x - 0.06) * 0.18;

	color += vec3(specular);
	return color;
}

vec3 getNormal(vec2 uv) {
	vec2 size = vec2(1.0,0.0);
	vec2 texelSize = 1.0 / textureSize(texture1, 0);

	float p0 = texture(texture1, vec2(uv.x, uv.y+texelSize.y)).x;
	float p1 = texture(texture1, vec2(uv.x, uv.y-texelSize.y)).x;
	float p2 = texture(texture1, vec2(uv.x+texelSize.x, uv.y)).x;
	float p3 = texture(texture1, vec2(uv.x-texelSize.x, uv.y)).x;

	return cross(normalize(vec3(size.xy, p1-p0)), normalize(vec3(size.yx, p3-p2)));
}

void main()
{
	vec2 longlat = vec2((atan(fN.x, fN.y) / 3.1415926 + 1.0) * 0.5,
                        (asin(fN.z) / 3.1415926 + 0.5));
	vec2 tc = longlat*30.0;
	vec3 dyTex = vec3(texture(texture1, tc));
	vec3 dxTex = vec3(texture(noiseTexture, tc));
	//vec3 color = vec3(mix(dyTex, dxTex, 1.0));
	vec3 waveNormal = getNormal(tc);
	vec4 ray = normalize(model*vec4(fPosition,1.0) - vec4(cameraPos, 1.0));
	mat3 normalMatrix = transpose(inverse(mat3(model)));

	vec3 T = normalize(vec3(vec4(fTangent, 0.0) * model));
	vec3 N = normalize(vec3(vec4(fN, 0.0)) * normalMatrix);
	T = normalize(T - dot(T, N) * N);
	vec3 B = cross(N, T);

	vec3 v;
	vec3 fragPos = vec3(vec4(fPosition,1.0)*model).xyz;
	vec4 lightPos = vec4(lightPosition, 1.0);
	vec3 lightDir = normalize(vec4(fPosition,1.0)*model - lightPos).xyz;

	v.x = dot(lightDir, T);
	v.y = dot(lightDir, B);
	v.z = dot(lightDir, N);
	vec3 fL = -normalize(v);

	vec3 halfVector = vec3((vec4(fPosition, 1.0)*model - lightPos) + ray).xyz;
	v.x = dot(halfVector, T);
	v.y = dot(halfVector, B);
	v.z = dot(halfVector, N);
	vec3 fH = -normalize(v);

	vec3 dist = vec3(model*vec4(fPosition,1.0) - vec4(cameraPos, 1.0));
	vec3 color = getSeaColor(fragPos, waveNormal, fL, cameraPos, dist, fH);

	vec4 ambientProduct = vec4(0.2, 0.2, 0.2, 1.0)*vec4(0.2, 0.2, 1.0, 1.0);
	float shininess = 250.0;

	vec3 ambient = ambientProduct.xyz * seaColor.rgb;

	vec4 fLightSpace = vec4(fPosition, 1.0)*model*lightSpace;
	float shadow = shadowCalculation(fLightSpace);
	//FragColor = vec4(color, 1.0);
	//FragColor = vec4(ambient+diffuse+specular, 1.0);
	FragColor = vec4(ambient+(1.0-shadow)*(color), 1.0);
}
