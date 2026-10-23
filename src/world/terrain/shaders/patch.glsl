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
// How many levels coarser the neighbour across edges u = 0, u = 1, v = 0,
// v = 1 is (0: same or finer): quad_tree.c coarserGap, see patchStitch.
layout (location = 12) in ivec4 pNbr;

// Grid cells along a patch edge (QT_DIVISIONS in quad_tree.c).
const int PATCH_DIVISIONS = 10;

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

// Edge stitching. The quadtree has no 2:1 balance, so a patch edge can
// face a neighbour k levels coarser, whose edge runs straight between
// vertices every S = 2^k of this patch's cells: a vertex in between has to
// lie on that segment, not on the terrain, or the gap opens a skirt wall.
// Those vertices are on the level-(L-k) ancestor's grid (the neighbour's
// lattice, also across cube faces), counted from the ancestor's corner.
struct PatchStitch {
	vec2 uvA, uvB;   // the coarse vertices either side, in this patch's uv
	ivec4 addr;      // the level-(L-k) ancestor (detail lattice address)
	vec2 aA, aB;     // the same vertices, in the ancestor's uv (exact)
	float t;         // weight of B
};

// Edge with gap k, vertex at grid index n along it, this patch's node
// coordinate j along it: true when the vertex falls between coarse
// vertices, with r its offset past the previous one (grid cells) and i
// that one's index in the ancestor. Unsigned: 10 * (2^28 - 1) + 10 fits.
bool patchStitchEdge(int k, int n, int j, out int r, out int i)
{
	r = 0;
	i = 0;
	if (k <= 0)
		return false;
	uint S = 1u << uint(k);
	uint m = uint(PATCH_DIVISIONS) * (uint(j) & (S - 1u)) + uint(n);
	r = int(m & (S - 1u));
	i = int(m >> uint(k));
	return r != 0;
}

// The coarse segment grid vertex uv has to lie on, if any. A corner sits
// on two edges, but between coarse vertices on at most one of them.
bool patchStitch(vec2 uv, out PatchStitch ps)
{
	ivec2 g = ivec2(round(uv * float(PATCH_DIVISIONS)));
	int k = 0, r, i, along = -1;   // along: 0 = the edge runs along u, 1 = along v
	if (g.x == 0 || g.x == PATCH_DIVISIONS) {
		k = g.x == 0 ? pNbr.x : pNbr.y;
		if (patchStitchEdge(k, g.y, pAddr.w, r, i))
			along = 1;
	}
	if (along < 0 && (g.y == 0 || g.y == PATCH_DIVISIONS)) {
		k = g.y == 0 ? pNbr.z : pNbr.w;
		if (patchStitchEdge(k, g.x, pAddr.z, r, i))
			along = 0;
	}
	ps.uvA = ps.uvB = ps.aA = ps.aB = uv;
	ps.addr = pAddr;
	ps.t = 0.0;
	if (along < 0)
		return false;

	int S = 1 << k;
	int n = along == 0 ? g.x : g.y;
	float D = float(PATCH_DIVISIONS);
	vec2 a = vec2(float(n - r), float(n - r + S)) / D;   // this patch
	vec2 c = vec2(float(i), float(i + 1)) / D;           // the ancestor
	// Across the edge both share it: u (or v) is 0 or 1 in each.
	float across = along == 0 ? uv.y : uv.x;
	if (along == 0) {
		ps.uvA = vec2(a.x, across); ps.uvB = vec2(a.y, across);
		ps.aA = vec2(c.x, across);  ps.aB = vec2(c.y, across);
	} else {
		ps.uvA = vec2(across, a.x); ps.uvB = vec2(across, a.y);
		ps.aA = vec2(across, c.x);  ps.aB = vec2(across, c.y);
	}
	ps.addr = ivec4(pAddr.x, pAddr.y - k, pAddr.z >> k, pAddr.w >> k);
	ps.t = float(r) / float(S);
	return true;
}
