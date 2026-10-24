// Terrain detail below the baked height map's resolution (step 3b):
// fractal gradient noise on land, set up by terrain_detail.c from the
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
// Gradient table (terrain_detail.c): random 3D gradients on a 64^3 lattice
// that tiles (each octave also shifts it, so they do not line up). It is
// stored once per constant axis as a 2D array texture (layer = that axis,
// texels = the other two in increasing order, RG = their gradient
// components), so one textureGather per channel fetches an octave's four
// corners: integer hashing is slow on integrated GPUs.

uniform int detailOctave0;     // lattice level of the coarsest octave
uniform int detailOctaves;     // octave count; 0 = detail off
// The land's roughness spectrum (WorldConfig.h TERRAIN_ROUGH_*, the same
// the bake carries above the map's texel): octave amplitude A10 (lambda /
// 10 km)^H up to the knee and ~lambda^detailHLong above it, by ruggedness
// between the plains' and the mountains', and proportional to lambda below
// detailLambdaC (the slope stops growing).
uniform vec2 detailA10;        // m at a 10 km wavelength: plains, mountains
uniform vec2 detailH;          // plains, mountains
uniform vec2 detailKnee;       // m: plains, mountains
uniform float detailHLong;
// Mountain shape (step 4): by ruggedness, each octave goes from soft noise
// to ridged (sharp crests, rounded valleys), and is damped by the slope the
// coarser detail octaves give the ground under it, 1 / (1 + detailErode r
// slope^2): their steep flanks stay smoother, gentle ground keeps its
// detail (the "eroded fbm" look: gullies, scree flanks). Not the map's
// slope: big steep shapes (the peaks) keep all their detail.
uniform float detailErode;
uniform float detailLambdaC;   // m
// Ruggedness from the baked map: its slope as rise / run (|surfaceMap.xyz|
// x detailSlopeToRise) between detailRugged.x and .y, or its height
// (alt01) between detailAltRugged.x and .y (up to 0.6 from height alone).
uniform float detailSlopeToRise;
uniform vec2 detailRugged;
uniform vec2 detailAltRugged;
uniform float detailRadius;    // body radius, metres
uniform float detailPix;       // radians per pixel: 2 tan(fov/2) / H
uniform float detailTriPx;     // this pass's LOD target, px per triangle edge
uniform sampler2DArray detailGradTex;   // gradient table (see above)

#define DETAIL_TABLE 64
// Ridged octave: DETAIL_RIDGE_K (DETAIL_RIDGE_MEAN - 2|n - c|), c =
// DETAIL_RIDGE_C. Gradient noise is zero at every lattice point, so a
// ridge on |n| itself puts a crest through every one: a square grid of
// pyramids wherever the octave is otherwise quiet. Offset to the contour
// n = c, the crests wander between the lattice points instead. Zero mean
// (E|n - 0.1| = 0.1637 for this noise, measured) and within [-1, 1]
// (|n - 0.1| <= ~0.79), with about the soft noise's rms. Its gradient is
// up to 1.6x the noise's (terrain_detail.c's seam bound allows 1.8).
// |n - c| is softened to sqrt((n - c)^2 + k^2) - k, k at least
// DETAIL_RIDGE_SOFT and enough to round
// the crest over about DETAIL_RIDGE_RES of what draws the octave (a
// triangle edge for the geometry, a pixel per pixel): long octaves keep
// sharp crests, ones near the mesh's or the screen's resolution do not
// shade the crest as stripes along the triangle rows or hairlines.
#define DETAIL_RIDGE_C    0.1
#define DETAIL_RIDGE_MEAN 0.3274
#define DETAIL_RIDGE_K    0.8
#define DETAIL_RIDGE_SOFT 0.02
#define DETAIL_RIDGE_RES  0.5
// Gullies (detailGully): where the ground is rugged and sloping, an octave
// becomes stripes running down the slope under it (the map's and the
// coarser octaves', gullies included), so each octave's gullies lean into
// the coarser ones and branch: drainage, instead of gradient noise's
// meandering contour loops. Weight r x smoothstep(ONSET) of that slope
// (m/m). Each lattice corner sets the stripes' phase; where the four
// disagree they fork. Their blend is normalized (cosine and sine
// together), floored at NORM so forks fade instead of pinching. AMP 0.25:
// the noise's rms (0.177) and a steepest slope of 0.25 x 2 pi = 1.57 per
// cell, under the ridged octave's bound.
#define DETAIL_GULLY_ONSET vec2(0.05, 0.3)
#define DETAIL_GULLY_NORM  0.5
#define DETAIL_GULLY_AMP   0.25
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
//   below 2 px), at most DETAIL_PIXEL_OCTAVES octaves (the band holds 3).
//   Set from TERRAIN_DETAIL_PIXEL_OCTAVES (WorldConfig.h) when the program
//   is built; per-pixel octaves are expensive on integrated GPUs, so the
//   band stops short of the pixel and finer detail is left flat.
#define DETAIL_GEOM_FADE   vec2(1.5, 3.0)
#define DETAIL_VERT_FADE   vec2(2.0, 4.0)
#define DETAIL_FINE_FADE   vec2(0.5, 1.0)
#define DETAIL_PIX_FADE    vec2(1.0, 2.0)
#ifndef DETAIL_PIXEL_OCTAVES
#define DETAIL_PIXEL_OCTAVES 3
#endif

// Cube axes (0 = x, 1 = y, 2 = z) along a face's s and t node coordinates
// (faceA / faceB in quad_tree.c). The third axis is constant on the face:
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

// The table gradients at the four corners of octave n's lattice cell (s, t)
// on a face, in the face's s / t (components across a cube edge dropped).
void detailCorners(int face, int n, ivec2 cell, out vec2 g00, out vec2 g10, out vec2 g01, out vec2 g11)
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
}

// Gradient noise from the corners at fraction f: x = value in [-1, 1], yz =
// its gradient per lattice cell along s and t. Quintic interpolation, so the
// gradient is continuous too.
vec3 detailNoise(vec2 g00, vec2 g10, vec2 g01, vec2 g11, vec2 f)
{
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

// One octave n at lattice cell (s, t) + fraction f on a face (detailNoise).
vec3 detailOctave(int face, int n, ivec2 cell, vec2 f)
{
	vec2 g00, g10, g01, g11;
	detailCorners(face, n, cell, g00, g10, g01, g11);
	return detailNoise(g00, g10, g01, g11, f);
}

// Gully stripes from the corners at fraction f: phase 2 pi (distance across
// the slope in cells + a corner's random offset), side the unit vector
// across the slope (cell units). Same (x value, yz gradient per cell) as
// detailNoise. The offset is the corner gradient's components summed: a
// corner on a cube edge keeps only the one along the edge, the same on
// both faces, and every face's (s, t) is right-handed from outside, so
// "across the slope" agrees too: the value is continuous over the edges.
// The gradient leaves out the weights' own slope (the stripes dominate).
vec3 detailGully(vec2 g00, vec2 g10, vec2 g01, vec2 g11, vec2 f, vec2 side)
{
	const float TAU = 6.2831853;
	vec2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float d = dot(f, side);
	vec4 ph = TAU * (vec4(d, d - side.x, d - side.y, d - side.x - side.y)
	                 + 2.0 * vec4(g00.x + g00.y, g10.x + g10.y, g01.x + g01.y, g11.x + g11.y));
	vec4 wc = vec4((1.0 - w.x) * (1.0 - w.y), w.x * (1.0 - w.y), (1.0 - w.x) * w.y, w.x * w.y);
	vec2 cs = vec2(dot(wc, cos(ph)), dot(wc, sin(ph)));
	cs /= max(length(cs), DETAIL_GULLY_NORM);
	return DETAIL_GULLY_AMP * vec3(cs.x, -cs.y * TAU * side);
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

// Wavelength (m) of the coarsest octave at a point with cube radius qLen:
// the cube-to-sphere projection shrinks cells by 1/|q| toward the corners.
float detailWavelength0(float qLen)
{
	return exp2(1.0 - float(detailOctave0)) * detailRadius / qLen;
}

// The detail's spectrum at a point: a10 (m) and H by ruggedness, and the
// coastal ramp (coast, 0..1), which keeps |detail| <= the height above the
// sea (the noise is within [-1, 1], the octaves' sum within ampTotal and
// smoothstep(t) <= 2t), so land never dips below it and the per-pixel
// coastline (quad.frag, water.frag) still matches the geometry.
struct DetailSpec {
	float a10, H, knee, coast;
	float r;        // ruggedness, 0 plains .. 1 mountains
};

// Octave amplitude (m) at wavelength lambda (m).
float detailAmpAt(DetailSpec s, float lambda)
{
	float l = max(lambda, detailLambdaC);
	return s.a10 * pow(min(l, s.knee) * 1e-4, s.H) * pow(max(l, s.knee) / s.knee, detailHLong)
	     * (lambda / l);
}

// Land hAboveM metres above sea level, alt01 of the relief up, where the
// baked map's slope is mapSlope (|surfaceMap.xyz|, raw units per radian),
// at cube radius qLen (sets the coarsest wavelength).
DetailSpec detailSpec(float hAboveM, float mapSlope, float alt01, float qLen)
{
	DetailSpec s = DetailSpec(0.0, 1.0, 1e9, 0.0, 0.0);
	if (detailOctaves <= 0)
		return s;
	float r = max(smoothstep(detailRugged.x, detailRugged.y, mapSlope * detailSlopeToRise),
	              0.6 * smoothstep(detailAltRugged.x, detailAltRugged.y, alt01));
	s.r = r;
	s.a10 = exp(mix(log(detailA10.x), log(detailA10.y), r));
	s.H = mix(detailH.x, detailH.y, r);
	s.knee = exp(mix(log(detailKnee.x), log(detailKnee.y), r));
	// The sum over the octaves (a bound): those above the knee at most the
	// coarsest's each, then geometric from the knee (ratio 2^-H, H <= 1;
	// the octaves under detailLambdaC fall faster).
	float lambda0 = detailWavelength0(qLen);
	float total = detailAmpAt(s, min(lambda0, s.knee)) / (1.0 - exp2(-s.H))
	            + detailAmpAt(s, lambda0) * max(ceil(log2(lambda0 / s.knee)), 0.0);
	s.coast = smoothstep(0.0, 1.0, hAboveM / (2.0 * max(total, 1e-3)));
	return s;
}

// One octave (detailOctave's value, gradient) shaped by ruggedness r: soft
// on plains, ridged in mountains. res: the octave's wavelength over the
// size of what draws it (triangle edge or pixel), for the crest's rounding.
vec3 detailShape(vec3 o, float r, float res)
{
	float k = max(DETAIL_RIDGE_SOFT, DETAIL_RIDGE_RES / max(res, 1e-3));
	float nc = o.x - DETAIL_RIDGE_C;
	float a = sqrt(nc * nc + k * k);
	vec3 ridge = DETAIL_RIDGE_K * vec3(DETAIL_RIDGE_MEAN + 2.0 * k - 2.0 * a,
	                                   -2.0 * (nc / a) * o.yz);
	return mix(o, ridge, r);
}

// One octave shaped for the ground (detailShape), turned into gullies on
// rugged slopes (detailGully). dirS: the slope under it, the map's and the
// coarser octaves' (metres per cube unit); toRise: metres per cube unit ->
// m/m.
vec3 detailFeature(int face, int n, ivec2 cell, vec2 f, DetailSpec s, float res, vec2 dirS, float toRise)
{
	vec2 g00, g10, g01, g11;
	detailCorners(face, n, cell, g00, g10, g01, g11);
	vec3 o = detailShape(detailNoise(g00, g10, g01, g11, f), s.r, res);
	float len = length(dirS);
	float wG = s.r * smoothstep(DETAIL_GULLY_ONSET.x, DETAIL_GULLY_ONSET.y, len * toRise);
	if (wG <= 0.0)
		return o;
	vec2 side = vec2(-dirS.y, dirS.x) / len;
	return mix(o, detailGully(g00, g10, g01, g11, f, side), wG);
}

// Amplitude factor for an octave where the coarser octaves' slope^2 is d2
// (m/m): steep rugged ground keeps less of its finer detail.
float detailErosion(DetailSpec s, float d2)
{
	return 1.0 / (1.0 + detailErode * s.r * d2);
}

// Octave index (0 = coarsest) of the finest octave at least `minLambda`
// long, given the coarsest's wavelength lambda0 (may be < 0: none).
int detailLastOctave(float lambda0, float minLambda)
{
	return min(int(floor(log2(lambda0 / max(minLambda, 1e-6)))), detailOctaves - 1);
}

// Geometric detail at a vertex footM metres per pixel from the camera:
// height in metres (coastal ramp applied). gradCube gets the slope of the
// octaves long enough to interpolate between vertices (metres per cube
// unit along the face's s and t axes); quad.frag adds shorter ones per
// pixel. Coarse to fine, each octave's lattice point found on its own
// (exact, as detailLattice is): the erosion and the gullies need the
// coarser octaves' slope. mapCube: the map's slope (metres per cube unit,
// detailCubeGrad), which the gullies follow too.
float detailVertex(ivec4 addr, vec2 uv, float qLen, float footM, DetailSpec s, vec2 mapCube,
                   out vec2 gradCube)
{
	gradCube = vec2(0.0);
	if (s.coast <= 0.0)
		return 0.0;
	float lambda0 = detailWavelength0(qLen);
	float tri = max(footM * detailTriPx, 1e-6);
	int last = detailLastOctave(lambda0, DETAIL_GEOM_FADE.x * tri);
	if (last < 0)
		return 0.0;
	float toRise = qLen / detailRadius;   // metres per cube unit -> m/m
	vec2 slope = vec2(0.0);               // the octaves so far, metres per cube unit
	float lambda = lambda0;
	float h = 0.0;
	for (int i = 0; i <= last; i++) {
		int n = detailOctave0 + i;
		ivec2 cell;
		vec2 f;
		detailLattice(addr, uv, n, cell, f);
		float r = lambda / tri;
		vec3 o = detailFeature(addr.x, n, cell, f, s, r, mapCube + s.coast * slope, toRise);
		float amp = detailAmpAt(s, lambda) * detailErosion(s, dot(slope, slope) * toRise * toRise);
		float wg = smoothstep(DETAIL_GEOM_FADE.x, DETAIL_GEOM_FADE.y, r);
		// d/d(cube unit) = d/d(cell) * 2^n / 2 (a face is 2 units wide).
		vec2 g = amp * exp2(float(n - 1)) * o.yz;
		h += wg * amp * o.x;
		slope += wg * g;
		gradCube += smoothstep(DETAIL_VERT_FADE.x, DETAIL_VERT_FADE.y, r) * g;
		lambda *= 0.5;
	}
	gradCube *= s.coast;
	return s.coast * h;
}

// The per-pixel share of the slope: the octaves detailVertex does not
// interpolate, down to half a triangle edge (metres per cube unit along s
// and t; coastal ramp NOT applied). footM: metres per pixel for the
// vertex/pixel split (same measure as detailVertex); pixM: the pixel
// footprint for aliasing, which can be wider (grazing views). d2: the
// vertices' detail slope^2 (m/m), where the erosion starts; startCube: the
// map's and the vertices' slope (metres per cube unit), where the gullies
// start.
vec2 detailPixel(ivec4 addr, vec2 uv, float qLen, float footM, float pixM, DetailSpec s, float d2,
                 vec2 startCube)
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
	float toRise = qLen / detailRadius;
	vec2 slope = vec2(0.0);
	float lambda = lambda0 * exp2(-float(first));
	for (int i = first; i <= last; i++) {
		float r = lambda / tri;
		float w = smoothstep(DETAIL_PIX_FADE.x, DETAIL_PIX_FADE.y, lambda / pixM)
		        * smoothstep(DETAIL_FINE_FADE.x, DETAIL_FINE_FADE.y, r)
		        * (1.0 - smoothstep(DETAIL_VERT_FADE.x, DETAIL_VERT_FADE.y, r));
		int n = detailOctave0 + i;
		ivec2 cell;
		vec2 f;
		detailLattice(addr, uv, n, cell, f);
		vec3 o = detailFeature(addr.x, n, cell, f, s, lambda / pixM, startCube + s.coast * slope, toRise);
		float amp = detailAmpAt(s, lambda) * detailErosion(s, d2 + dot(slope, slope) * toRise * toRise);
		vec2 g = w * amp * exp2(float(n - 1)) * o.yz;
		grad += g;
		slope += g;
		lambda *= 0.5;
	}
	return grad;
}

// Variance of the slope (m/m, both axes together) of the octaves finer than
// detailVertex and detailPixel draw: what a pixel holds but does not shade,
// for the materials that go by slope (quad.frag), so rock and snow cover
// the same share of a range from orbit as from the ground. Same arguments
// as detailPixel; d2: the shaded detail's slope^2 (m/m), where the erosion
// starts. One octave's mean square gradient per lattice cell, measured:
// 0.514 soft, 1.316 ridged; a cell is one wavelength. Coastal ramp NOT
// applied.
float detailUnresolvedVar(float qLen, float footM, float pixM, DetailSpec s, float d2)
{
	if (detailOctaves <= 0)
		return 0.0;
	float lambda0 = detailWavelength0(qLen);
	float tri = max(footM * detailTriPx, 1e-6);
	int first = max(detailLastOctave(lambda0, DETAIL_VERT_FADE.y * tri) + 1, 0);
	int last = min(detailLastOctave(lambda0, max(DETAIL_FINE_FADE.x * tri, DETAIL_PIX_FADE.x * pixM)),
	               first + DETAIL_PIXEL_OCTAVES - 1);
	int i0 = max(last, first - 1) + 1;
	float g2 = mix(0.514, 1.316, s.r);
	float var = 0.0;
	float lambda = lambda0 * exp2(-float(i0));
	// amp / lambda stepped per halving (2^(1 - H) above detailLambdaC, flat
	// below); the knee taken at the first octave only (these are short).
	float a = detailAmpAt(s, lambda) / lambda;
	float stepH = exp2(1.0 - s.H);
	float k = detailErode * s.r;
	for (int i = i0; i < detailOctaves; i++) {
		float ae = a / (1.0 + k * (d2 + var));
		var += g2 * ae * ae;
		a *= lambda > detailLambdaC ? stepH : 1.0;
		lambda *= 0.5;
	}
	return var;
}

// detailSlope's inverse: a body-local surface slope (m/m) at cube point q
// -> metres per cube unit along the face's s and t axes.
vec2 detailCubeGrad(int face, vec3 q, vec3 slope)
{
	ivec2 st = DETAIL_FACE_ST[face];
	vec3 A = vec3(equal(ivec3(0, 1, 2), ivec3(st.x)));
	vec3 B = vec3(equal(ivec3(0, 1, 2), ivec3(st.y)));
	float qLen = length(q);
	vec3 d = q / qLen;
	vec3 a = A - d * dot(d, A);
	vec3 b = B - d * dot(d, B);
	return vec2(dot(slope, a), dot(slope, b)) * (detailRadius / qLen);
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
