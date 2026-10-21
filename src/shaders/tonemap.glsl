// The scene's tone map (ACES fit, exposure, gamma 2.2), shared by every lit
// shader so they all go through the same curve: the scene target holds
// display values (RGBA8), and the atmosphere dome adds on top of them.
const float EXPOSURE = 1.05;
const float GAMMA = 2.2;
vec3 toneMap(vec3 c)
{
	vec3 x = c * EXPOSURE;
	x = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	return pow(x, vec3(1.0 / GAMMA));
}

// Inverse of toneMap. The dome and skybox are added straight to the
// framebuffer (display values, never tone mapped), so a reflected sky
// converts back to linear radiance here and comes out of toneMap looking
// like the sky it mirrors.
vec3 displayToLinear(vec3 d)
{
	vec3 y = pow(clamp(d, 0.0, 0.95), vec3(GAMMA));
	vec3 a = 2.51 - 2.43 * y;
	vec3 b = 0.03 - 0.59 * y;
	vec3 x = (-b + sqrt(b * b + 4.0 * a * 0.14 * y)) / (2.0 * a);
	return x / EXPOSURE;
}
