#version 410

in vec4 fLightSpace;
in vec3 fTangent;
in vec3 fragPos;
in vec3 fPosition;
in vec3 fN;
in vec3 fT;
in vec3 fB;
in vec3 lightDir;
in vec3 vColor;
in vec3 waveNormal;
in vec2 fTexCoords;

out vec4 FragColor;

uniform mat4 model;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
uniform sampler2D depthMap;
uniform sampler2D noiseTexture;
uniform mat4 lightSpace;

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

void main()
{
	vec3 seaColor = vec3(0.1,0.19,0.22);
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

	vec4 ambientProduct = vec4(0.2, 0.2, 0.2, 1.0)*vec4(0.2, 0.2, 1.0, 1.0);
	vec4 diffuseProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.8, 0.8, 0.8, 1.0);
	vec4 specularProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.9, 0.9, 0.9, 1.0);
	float shininess = 250.0;

	float Kd = max(dot(fL, waveNormal), 0.0);
	float Ks = pow(max(dot(waveNormal, fH), 0.0), shininess);
	vec3 ambient = ambientProduct.xyz * seaColor.rgb;
	vec3 diffuse = Kd * diffuseProduct.xyz * seaColor.rgb;
	vec3 specular = Ks * specularProduct.xyz;

	if(dot(fL, waveNormal) < 0.0) {
		specular = vec3(0.0, 0.0, 0.0);
	}
	vec4 fLightSpace = vec4(fPosition, 1.0)*model*lightSpace;
	float shadow = shadowCalculation(fLightSpace);
	//FragColor = vec4(vColor, 1.0);
	//FragColor = vec4(ambient+diffuse+specular, 1.0);
	FragColor = vec4(ambient+(1.0-shadow)*(diffuse+specular), 1.0);
}
