// Quadtree patch vertex positions (quad_tree.c), shared by quad.vert,
// water.vert, depth.vert and light.vert so the caster, the surface and the
// water shell are always the same mesh.
//
// A patch is a square on one cube face: q(u,v) = Q0 + u*Eu + v*Ev in
// body-local cube space, and the surface is R * normalize(q). The base
// grid vertex is just (u, v) in [0, 1] (vPosition.xy; .w is the skirt flag).
//
// Precision: the CPU computes the four patch corners on the sphere in
// double, camera-relative (C_i = model * normalize(q_i)), so near the
// camera they are small numbers. Any surface point is rebuilt from them
// exactly, without ever forming a planet-sized coordinate:
//   normalize(q) = normalize(m),  m = sum w_i n_i,  w_i = bilinear_i(u,v)*|q_i|
//   (n_i = corner directions; the |q_i| factor makes the blend match the
//   cube projection exactly), and with the weights normalised to sum 1
//   P = sum w_i C_i + R * m^ * (1 - |m|)
//   |m|^2 = 1 - eps,  eps = sum_{i<j} w_i w_j |C_i - C_j|^2 / R^2
//   1 - |m| = eps / (1 + sqrt(1 - eps))     (no cancellation)
// This is an identity, not an approximation, so one path serves every
// patch size.

layout (location = 4) in vec4 pC0;   // xyz: corner at (0,0), camera-relative metres; w: skirt drop
layout (location = 5) in vec4 pC1;   // (1,0)
layout (location = 6) in vec4 pC2;   // (0,1)
layout (location = 7) in vec4 pC3;   // (1,1)
layout (location = 8) in vec4 pQ0;   // xyz: cube-space corner (0,0); w: cell half-width (cube units)
layout (location = 9) in vec3 pEu;   // cube-space edge along u
layout (location = 10) in vec3 pEv;  // cube-space edge along v
// Integer node address (face, level, x, y): exact lattice maths for the
// terrain detail noise (detail.glsl).
layout (location = 11) in ivec4 pAddr;

// 1 = the old float path (normalize, then the model matrix): debug panel
// comparison only. It loses ~1e-7 of the radius (~0.5-1 m on Earth).
uniform int patchLegacy;

// Body-local cube-space point of patch point uv (not normalised).
vec3 patchCube(vec2 uv)
{
	return pQ0.xyz + uv.x * pEu + uv.y * pEv;
}

// Patch cell half-width in cube units: the height-map mip for the node.
float patchCell()
{
	return pQ0.w;
}

// How far (body radii) this patch's skirt wall hangs below its surface:
// sized per patch by the walk from its height range (skirtDrop in
// quad_tree.c, whose culling bounds include it).
float patchSkirt()
{
	return pC0.w;
}

// Body-local unit direction of patch point uv.
vec3 patchDir(vec2 uv)
{
	return normalize(pQ0.xyz + uv.x * pEu + uv.y * pEv);
}

// Camera-relative position of the undisplaced sphere point at uv (dir =
// patchDir(uv)). Row-vector convention: world = vec4(local, 1) * model.
vec3 patchSphere(vec2 uv, vec3 dir, mat4 model)
{
	if (patchLegacy == 1)
		return (vec4(dir, 1.0) * model).xyz;

	vec3 q1 = pQ0.xyz + pEu;
	vec3 q2 = pQ0.xyz + pEv;
	vec3 q3 = q1 + pEv;
	vec4 w = vec4((1.0 - uv.x) * (1.0 - uv.y), uv.x * (1.0 - uv.y),
	              (1.0 - uv.x) * uv.y, uv.x * uv.y)
	       * vec4(length(pQ0.xyz), length(q1), length(q2), length(q3));
	w /= w.x + w.y + w.z + w.w;

	vec3 base = w.x * pC0.xyz + w.y * pC1.xyz + w.z * pC2.xyz + w.w * pC3.xyz;

	vec3 d01 = pC0.xyz - pC1.xyz, d02 = pC0.xyz - pC2.xyz, d03 = pC0.xyz - pC3.xyz;
	vec3 d12 = pC1.xyz - pC2.xyz, d13 = pC1.xyz - pC3.xyz, d23 = pC2.xyz - pC3.xyz;
	mat3 M = mat3(model);   // R * rotation (uniform scale)
	float eps = (w.x * w.y * dot(d01, d01) + w.x * w.z * dot(d02, d02)
	           + w.x * w.w * dot(d03, d03) + w.y * w.z * dot(d12, d12)
	           + w.y * w.w * dot(d13, d13) + w.z * w.w * dot(d23, d23))
	          / dot(M[0], M[0]);
	float sag = eps / (1.0 + sqrt(max(1.0 - eps, 0.0)));
	return base + (dir * sag) * M;
}

// A body-local offset (in body radii: relief along dir, waves, skirts) as a
// camera-relative world vector. Only small numbers are involved.
vec3 patchOffset(vec3 localOffset, mat4 model)
{
	return localOffset * mat3(model);
}
