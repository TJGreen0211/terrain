#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 lightSpace;
uniform vec3 lightPosition;
uniform float time;
uniform sampler2D texture1;
uniform sampler2D depthMap;
uniform sampler2D noiseTexture;

out vec4 fLightSpace;
out vec3 fPosition;
out vec3 fragPos;
out vec3 fTangent;
out vec3 fN;
out vec3 vColor;

out vec2 fTexCoords;

const float PI	 	= 3.141592;

/*
float hash( vec2 p ) {
    float h = dot(p,vec2(127.1,311.7));
    return fract(sin(h)*43758.5453123);
}

float noise( in vec2 p ) {
    vec2 i = floor( p );
    vec2 f = fract( p );
    vec2 u = f*f*(3.0-2.0*f);
    return -1.0+2.0*mix(
                mix( hash( i + vec2(0.0,0.0) ), hash( i + vec2(1.0,0.0) ), u.x),
                mix( hash( i + vec2(0.0,1.0) ), hash( i + vec2(1.0,1.0) ), u.x),
                u.y);
}

float gerstnerWaves(vec2 uv, float A, float L, vec2 K) {
	vec2 wave = vec2(0.0);
	uv += noise(uv);
	float k = 2.0*PI/L;
	float omega = sqrt(9.81/k);
	//wave.x = (uv - (K/k)*A*sin(dot(K, uv) - omega*time*10.0)).y;
	wave.x = ((K/k)*A*sin(dot(K, uv) - omega*time*50.0)).y;
	wave.y = A*cos(dot(K, uv) - omega*time*50.0);

	float wv = (wave.x * (1 - 0.5) + wave.y * 0.2);
	return wv;//pow(pow(wv,1.5),0.9);
}*/

void main()
{
	fTangent = vTangent;
	fPosition = vPosition.xyz;
	vec4 vWavePosition = vPosition;

	vec2 longlat = vec2((atan(vWavePosition.x, vWavePosition.y) / 3.1415926 + 1.0) * 0.5,
                        (asin(vWavePosition.z) / 3.1415926 + 0.5));
	vec2 tc = longlat*3.0;
	vec3 dyTex = vec3(texture(texture1, tc));
	vec3 dxTex = vec3(texture(noiseTexture, tc));
	vec3 wavePosition = mix(dyTex, dxTex, 1.0);
	vec3 heightVec = (vWavePosition.xyz * wavePosition)/50.0;
	vWavePosition.xyz = vec3(vWavePosition.x+heightVec.x, vWavePosition.y+heightVec.y, vWavePosition.z+heightVec.z);
	vec4 color = vec4(wavePosition, 1.0);

	fN = vPosition.xyz;
	fLightSpace = vWavePosition*model*lightSpace;
	fragPos = vec3(vWavePosition*model).xyz;
	fTexCoords = longlat;
	vColor = vec3(color.xyz);

	gl_Position = vWavePosition*model*view*projection;
}
