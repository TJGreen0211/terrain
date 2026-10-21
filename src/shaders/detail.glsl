// Terrain detail below the baked height map's resolution (step 3b):
// fractal gradient noise on land, set up by TerrainDetail.c from the
// TERRAIN_DETAIL_* constants in WorldConfig.h. Shared by quad.vert and
// depth.vert (geometry) and quad.frag (shading normals).
//
// Domain: the noise lives on the cube surface, on one integer lattice per
// octave (octave n has 2^n cells across a face). A point's lattice cell
// comes from exact integer maths on its quadtree node address (face, level,
// x, y) and only the fraction inside the cell is a float, so it resolves
// sub-metre detail anywhere on an Earth-size body (a float direction only
// resolves ~0.6 m). Neighbouring patches, and the faces meeting at a cube
// edge, address the same lattice points, so the field is continuous
// everywhere. Each face sees 2D gradient noise in its own plane; the
// corner gradients come from one 3D table keyed on the lattice point so
// faces agree along edges, and they lose their component across a cube
// edge, so the slope is continuous over it too (no crease along the 12
// edges).
//
// Gradient table (TerrainDetail.c): random 3D gradients on a 64^3 lattice
// that tiles (each octave also shifts it, so they do not line up). It is
// stored once per constant axis as a 2D array texture (layer = that axis,
// texels = the other two in increasing order, RG = their gradient
// components), so one textureGather per channel fetches an octave's four
// corners: integer hashing is slow on integrated GPUs.

uniform int detailOctave0;     // lattice level of the coarsest octave
uniform int detailOctaves;     // octave count; 0 = detail off
uniform float detailAmp0;      // coarsest octave amplitude, metres
uniform float detailGain;      // amplitude ratio per octave (wavelength halves)
uniform float detailAmpTotal;  // sum of every octave's amplitude, metres
uniform float detailPlains;    // amplitude on low land relative to high ground
uniform float detailRadius;    // body radius, metres
uniform float detailPix;       // radians per pixel: 2 tan(fov/2) / H
uniform float detailTriPx;     // this pass's LOD target, px per triangle edge
uniform sampler2DArray detailGradTex;   // gradient table (see above)

#define DETAIL_TABLE 64
// Per-octave lattice shift (times n), so octaves sample different tiles.
const ivec3 DETAIL_SHIFT = ivec3(17, 31, 47);

// Octave fades, by the octave's wavelength in triangle edges (at this
// pass's LOD target) or in pixels:
// - geometry: fully in at 3 triangle edges, gone below 1.5 (the vertices
//   would alias it, and pop as patches split). Distance-based, so a vertex
//   shared by two patches gets the same height from both (no cracks).
// - vertex slope: the slope is interpolated between vertices for octaves
//   of 4+ triangle edges, and fades to per-pixel below that.
// - per-pixel slope: from there down to half a triangle edge (and never
//   below 2 px), at most DETAIL_PIXEL_OCTAVES octaves. Every octave per
//   pixel costs roughly what the ~50 cheapest lines of quad.frag do, so
//   the band stops short of the pixel; finer detail is left flat.
#define DETAIL_GEOM_FADE   vec2(1.5, 3.0)
#define DETAIL_VERT_FADE   vec2(2.0, 4.0)
#define DETAIL_FINE_FADE   vec2(0.5, 1.0)
#define DETAIL_PIX_FADE    vec2(1.0, 2.0)
#define DETAIL_PIXEL_OCTAVES 3

// Cube axes (0 = x, 1 = y, 2 = z) along a face's s and t node coordinates
// (faceA / faceB in QuadTree.c). The third axis is constant on the face:
// face >> 1, at +1 for even faces and -1 for odd ones.
const ivec2 DETAIL_FACE_ST[6] = ivec2[6](
	ivec2(1, 2), ivec2(2, 1),   // +X, -X
	ivec2(2, 0), ivec2(0, 2),   // +Y, -Y
	ivec2(0, 1), ivec2(1, 0));  // +Z, -Z

// Lattice cell and fraction along one face axis for node coordinate x at
// level L plus uv coordinate u, at octave n. Integer part exact; the
// fraction is a small float either way.
void detailAxis(int x, float u, int L, int n, out int cell, out float frac)
{
	int k = n - L;
	if (k <= 0) {
		int sh = -k;
		cell = x >> sh;
		frac = (float(x & ((1 << sh) - 1)) + u) * exp2(float(-sh));
	} else {
		float c = u * exp2(float(k));
		float fl = floor(c);
		cell = (x << k) + int(fl);
		frac = c - fl;
	}
}

// Lattice cell (s, t) and fraction of node point uv at octave n.
void detailLattice(ivec4 addr, vec2 uv, int n, out ivec2 cell, out vec2 frac)
{
	detailAxis(addr.z, uv.x, addr.y, n, cell.x, frac.x);
	detailAxis(addr.w, uv.y, addr.y, n, cell.y, frac.y);
}

// The same point one octave coarser. Exact up to one rounding of the
// fraction, so loops run from fine to coarse (the other way, the
// fraction's rounding error would double every octave).
void detailCoarsen(inout ivec2 cell, inout vec2 frac)
{
	frac = (frac + vec2(cell & ivec2(1))) * 0.5;
	cell >>= 1;
}

// One octave n at lattice cell (s, t) + fraction f on a face: x = value in
// [-1, 1], yz = its gradient per lattice cell along s and t. Quintic
// interpolation, so the gradient is continuous too.
vec3 detailOctave(int face, int n, ivec2 cell, vec2 f)
{
	ivec2 st = DETAIL_FACE_ST[face];
	int is = cell.x, it = cell.y;

	// Table lookup: texel (a1, a2) of layer (axis, a0) for the constant axis
	// a0 and the others a1 < a2; this face's s/t are those, maybe swapped.
	int top = 1 << n;
	int ca = face >> 1;
	ivec3 off = n * DETAIL_SHIFT;
	int layer = ca * DETAIL_TABLE
	          + ((((face & 1) == 0 ? top : 0) + off[ca]) & (DETAIL_TABLE - 1));
	int ts = (is + off[st.x]) & (DETAIL_TABLE - 1);
	int tt = (it + off[st.y]) & (DETAIL_TABLE - 1);
	bool swap = st.x > st.y;
	vec2 texel = swap ? vec2(tt, ts) : vec2(ts, tt);
	// At the shared corner of texels (i, i+1): gather returns
	// x = (i, j+1), y = (i+1, j+1), z = (i+1, j), w = (i, j); REPEAT wraps.
	vec3 tc = vec3((texel + 1.0) / float(DETAIL_TABLE), float(layer));
	vec4 R = textureGather(detailGradTex, tc, 0);
	vec4 G = textureGather(detailGradTex, tc, 1);
	vec2 g00, g10, g01, g11;
	if (!swap) {
		g00 = vec2(R.w, G.w); g10 = vec2(R.z, G.z);
		g01 = vec2(R.x, G.x); g11 = vec2(R.y, G.y);
	} else {
		g00 = vec2(G.w, R.w); g10 = vec2(G.x, R.x);
		g01 = vec2(G.z, R.z); g11 = vec2(G.y, R.y);
	}
	// Corners on a cube edge drop the component across it.
	vec2 m0 = vec2((is == 0 || is == top) ? 0.0 : 1.0, (it == 0 || it == top) ? 0.0 : 1.0);
	vec2 m1 = vec2(is + 1 == top ? 0.0 : 1.0, it + 1 == top ? 0.0 : 1.0);
	g00 *= m0;
	g10 *= vec2(m1.x, m0.y);
	g01 *= vec2(m0.x, m1.y);
	g11 *= m1;

	float v00 = dot(g00, f);
	float v10 = dot(g10, f - vec2(1.0, 0.0));
	float v01 = dot(g01, f - vec2(0.0, 1.0));
	float v11 = dot(g11, f - vec2(1.0, 1.0));

	vec2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec2 dw = 30.0 * f * f * (f * (f - 2.0) + 1.0);
	float k1 = v10 - v00, k2 = v01 - v00, k3 = v00 - v10 - v01 + v11;
	float value = v00 + k1 * w.x + k2 * w.y + k3 * w.x * w.y;
	vec2 grad = g00 + w.x * (g10 - g00) + w.y * (g01 - g00)
	          + w.x * w.y * (g00 - g10 - g01 + g11)
	          + dw * (vec2(k1, k2) + k3 * w.yx);
	return vec3(value, grad);
}

// Body-local cube point of node point uv (|q| sets the local wavelength
// and the slope frame; no precision needed beyond that).
vec3 detailCubePoint(ivec4 addr, vec2 uv)
{
	ivec2 st = DETAIL_FACE_ST[addr.x];
	float size = exp2(1.0 - float(addr.y));
	vec3 q = vec3(0.0);
	q[addr.x >> 1] = (addr.x & 1) == 0 ? 1.0 : -1.0;
	q[st.x] = -1.0 + (float(addr.z) + uv.x) * size;
	q[st.y] = -1.0 + (float(addr.w) + uv.y) * size;
	return q;
}

// Amplitude envelope (0..1) for land hAboveM metres above sea level at
// relative altitude alt01 (0 = shore, 1 = highest peaks). The coastal ramp
// keeps |detail| <= hAboveM (the noise is within [-1, 1] and
// smoothstep(t) <= 2t), so land never dips below the sea and the per-pixel
// coastline (quad.frag, water.frag) still matches the geometry. Low land is
// smoother than high ground.
float detailEnvelope(float hAboveM, float alt01)
{
	if (detailOctaves <= 0)
		return 0.0;
	float coast = smoothstep(0.0, 1.0, hAboveM / (2.0 * detailAmpTotal));
	return coast * mix(detailPlains, 1.0, smoothstep(0.0, 0.3, alt01));
}

// Wavelength (m) of the coarsest octave at a point with cube radius qLen:
// the cube-to-sphere projection shrinks cells by 1/|q| toward the corners.
float detailWavelength0(float qLen)
{
	return exp2(1.0 - float(detailOctave0)) * detailRadius / qLen;
}

// Octave index (0 = coarsest) of the finest octave at least `minLambda`
// long, given the coarsest's wavelength lambda0 (may be < 0: none).
int detailLastOctave(float lambda0, float minLambda)
{
	return min(int(floor(log2(lambda0 / max(minLambda, 1e-6)))), detailOctaves - 1);
}

// Geometric detail at a vertex footM metres per pixel from the camera:
// height in metres (envelope applied). gradCube gets the slope of the
// octaves long enough to interpolate between vertices (metres per cube
// unit along the face's s and t axes); quad.frag adds shorter ones per
// pixel.
float detailVertex(ivec4 addr, vec2 uv, float qLen, float footM, float env, out vec2 gradCube)
{
	gradCube = vec2(0.0);
	if (env <= 0.0)
		return 0.0;
	float lambda0 = detailWavelength0(qLen);
	float tri = max(footM * detailTriPx, 1e-6);
	int last = detailLastOctave(lambda0, DETAIL_GEOM_FADE.x * tri);
	if (last < 0)
		return 0.0;
	ivec2 cell;
	vec2 f;
	detailLattice(addr, uv, detailOctave0 + last, cell, f);
	float lambda = lambda0 * exp2(-float(last));
	float amp = detailAmp0 * pow(detailGain, float(last));
	float h = 0.0;
	for (int i = last; i >= 0; i--) {
		int n = detailOctave0 + i;
		vec3 o = detailOctave(addr.x, n, cell, f);
		float r = lambda / tri;
		h += smoothstep(DETAIL_GEOM_FADE.x, DETAIL_GEOM_FADE.y, r) * amp * o.x;
		// d/d(cube unit) = d/d(cell) * 2^n / 2 (a face is 2 units wide).
		gradCube += smoothstep(DETAIL_VERT_FADE.x, DETAIL_VERT_FADE.y, r)
		          * amp * exp2(float(n - 1)) * o.yz;
		detailCoarsen(cell, f);
		amp /= detailGain;
		lambda *= 2.0;
	}
	gradCube *= env;
	return env * h;
}

// The per-pixel share of the slope: the octaves detailVertex does not
// interpolate, down to half a triangle edge (metres per cube unit along s
// and t; envelope NOT applied). footM: metres per pixel for the
// vertex/pixel split (same measure as detailVertex); pixM: the pixel
// footprint for aliasing, which can be wider (grazing views).
vec2 detailPixel(ivec4 addr, vec2 uv, float qLen, float footM, float pixM)
{
	vec2 grad = vec2(0.0);
	if (detailOctaves <= 0)
		return grad;
	float lambda0 = detailWavelength0(qLen);
	float tri = max(footM * detailTriPx, 1e-6);
	// Band: shorter than the fully vertex-carried octaves, longer than the
	// fine and pixel fades.
	int first = max(detailLastOctave(lambda0, DETAIL_VERT_FADE.y * tri) + 1, 0);
	int last = min(detailLastOctave(lambda0, max(DETAIL_FINE_FADE.x * tri, DETAIL_PIX_FADE.x * pixM)),
	               first + DETAIL_PIXEL_OCTAVES - 1);
	if (last < first)
		return grad;
	ivec2 cell;
	vec2 f;
	detailLattice(addr, uv, detailOctave0 + last, cell, f);
	float lambda = lambda0 * exp2(-float(last));
	float amp = detailAmp0 * pow(detailGain, float(last));
	for (int i = last; i >= first; i--) {
		float r = lambda / tri;
		float w = smoothstep(DETAIL_PIX_FADE.x, DETAIL_PIX_FADE.y, lambda / pixM)
		        * smoothstep(DETAIL_FINE_FADE.x, DETAIL_FINE_FADE.y, r)
		        * (1.0 - smoothstep(DETAIL_VERT_FADE.x, DETAIL_VERT_FADE.y, r));
		int n = detailOctave0 + i;
		grad += w * amp * exp2(float(n - 1)) * detailOctave(addr.x, n, cell, f).yz;
		detailCoarsen(cell, f);
		amp /= detailGain;
		lambda *= 2.0;
	}
	return grad;
}

// Cube-unit gradient (metres per cube unit along the face's s and t axes)
// at cube point q -> body-local surface slope vector (m/m, tangent to the
// sphere): the surface normal is normalize(dir - slope).
vec3 detailSlope(int face, vec3 q, vec2 gradCube)
{
	ivec2 st = DETAIL_FACE_ST[face];
	vec3 A = vec3(equal(ivec3(0, 1, 2), ivec3(st.x)));
	vec3 B = vec3(equal(ivec3(0, 1, 2), ivec3(st.y)));
	float qLen = length(q);
	vec3 d = q / qLen;
	// Surface P = R q/|q|: dP/ds = (R/|q|) a, a = A projected off d (same
	// for t). Solve slope.a = gs |q|/R, slope.b = gt |q|/R in span(a, b).
	vec3 a = A - d * dot(d, A);
	vec3 b = B - d * dot(d, B);
	float aa = dot(a, a), ab = dot(a, b), bb = dot(b, b);
	vec2 r = gradCube * (qLen / detailRadius);
	float inv = 1.0 / (aa * bb - ab * ab);
	return ((bb * r.x - ab * r.y) * a + (aa * r.y - ab * r.x) * b) * inv;
}
