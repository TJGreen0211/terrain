#version 410

in vec2 fTexCoords;
in vec3 fragPos;
in vec3 fE;
in vec3 fN;
in vec3 fL;
in vec3 fH;

out vec4 FragColor;

uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
uniform sampler2DShadow depthMap;   // see quad.frag sunVisibility
uniform mat4 lightSpace;
uniform float shadowTexel;

// Sun visibility from the shadow map (same lookup as quad.frag): 1 = lit.
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

void main()
{   
	vec4 ambientProduct = vec4(0.2, 0.2, 0.2, 1.0)*vec4(0.2, 0.2, 1.0, 1.0);
	vec4 diffuseProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.8, 0.8, 0.8, 1.0);
	vec4 specularProduct = vec4(1.0, 1.0, 1.0, 1.0)*vec4(0.5, 0.5, 0.5, 1.0);
	float shininess = 25.0;
	
	vec3 ray = normalize(fragPos - cameraPos);
	vec3 fH = -normalize((fragPos - lightPosition) + ray).xyz;
	
	float Kd = max(dot(fL, fN), 0.0);
	float Ks = pow(max(dot(fN, fH), 0.0), shininess);
	//vec3 color = vec3(texture(texture1, fTexCoords));
	vec4 color = vec4(texture(texture1, fTexCoords)).rgba;
	vec3 ambient = ambientProduct.xyz * color.rgb;
	vec3 diffuse = Kd * diffuseProduct.xyz * color.rgb;
	vec3 specular = Ks * specularProduct.xyz;
	if(dot(fL, fN) < 0.0) {
		specular = vec3(0.0, 0.0, 0.0);
	}

	vec3 lighting = (ambient * (diffuse+specular));
	float gamma = 2.2;
	float vis = sunVisibility(fragPos, normalize(fN), normalize(fL));
	FragColor = vec4(pow(ambient+vis*(diffuse+specular), vec3(1.0/gamma)), color.a);
	//vec3 lighting = (ambient+(1.0-shadow) * (diffuse+specular));
	//FragColor = vec4(vec3(shadow), 1.0);
}
