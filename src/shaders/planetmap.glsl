// Baked planet maps (TerrainGen.c): the height, climate and ocean fields are
// cube maps over the body-local unit sphere, in GL's face layout with the
// quadtree's plain (gnomonic) projection, so a body-local direction is the
// lookup coordinate as it is (seamless filtering across the face edges).
//
// Mip selection: callers size a footprint against a face-centre texel
// (planetMapTexel radians at mip 0; the CPU's lodK / lodCellK use the same).
// Texels shrink toward the face corners, their smallest extent by m^2
// (m = the direction's major component, 1/sqrt(3) at a corner), so
// planetMapLodBias moves the mip up by up to log2(3) there. TerrainGen.c's
// height bounds add the same bias.

float planetMapTexel(samplerCube map)
{
	return 2.0 / float(textureSize(map, 0).x);
}

float planetMapMaxLod(samplerCube map)
{
	return log2(float(textureSize(map, 0).x));
}

float planetMapLodBias(vec3 d)
{
	vec3 a = abs(d);
	float m = max(a.x, max(a.y, a.z)) / length(d);
	return -log2(m * m);
}

// Channel 0 of a planet map at lod, filtered at full float precision close
// up. Hardware filtering quantizes the sub-texel weights to 8 bits: with
// ~12 km texels at Earth's size the field it returns is a staircase of
// ~50 m steps, which drew the coastline (the height field's sea-level
// contour) as stair steps and broke the shore foam into dots. Below
// lod 1 this filters the base level's 4x4 texels by hand (four
// textureGathers, weights from the face coordinate, GL's cube face table);
// from lod 1 up the steps are below a pixel and the hardware filter stands.
// quad.frag and water.frag both read the height through this, so the land
// and the water discard keep meeting on the same contour.
//
// Catmull-Rom, not bilinear, in texels whose 2x2 block reaches into band
// (heights; the coast band, seastate.glsl seaCoastBand): it still passes
// through the texel values, but its slope is continuous. The shore effects
// measure the distance to the coast as depth / seabed slope, and the
// bilinear slope jumps at every texel edge, which bent the surf's wave
// fronts into kinks every ~12 km. Elsewhere the one gather's bilinear
// result stands (4x cheaper; the choice is per texel and the same in every
// shader, so the land and water still agree pixel for pixel).
//
// Each 2x2 block is chosen here and gathered at the exact point between its
// texels: gathering at d itself let the hardware's fixed-point coordinate
// pick a different block than floor() along texel rows and columns, which
// drew the texel grid as thin dark lines. Blocks stay inside the face (the
// last texel and a half at a face edge is held flat).
vec3 planetMapFaceDir(int face, vec2 st)
{
	// GL's cube face table, inverted: st = (sc, tc) / |ma| in [-1, 1].
	if (face == 0) return vec3( 1.0, -st.y, -st.x);
	if (face == 1) return vec3(-1.0, -st.y,  st.x);
	if (face == 2) return vec3( st.x,  1.0,  st.y);
	if (face == 3) return vec3( st.x, -1.0, -st.y);
	if (face == 4) return vec3( st.x, -st.y,  1.0);
	return vec3(-st.x, -st.y, -1.0);
}

float planetMapFine(samplerCube map, vec3 d, float lod, vec2 band)
{
	float hw = textureLod(map, d, lod).r;
	if (lod >= 1.0)
		return hw;
	vec3 a = abs(d);
	int face;
	float ma;
	vec2 st;
	if (a.x >= a.y && a.x >= a.z) {
		ma = a.x;
		face = d.x > 0.0 ? 0 : 1;
		st = d.x > 0.0 ? vec2(-d.z, -d.y) : vec2(d.z, -d.y);
	} else if (a.y >= a.z) {
		ma = a.y;
		face = d.y > 0.0 ? 2 : 3;
		st = d.y > 0.0 ? vec2(d.x, d.z) : vec2(d.x, -d.z);
	} else {
		ma = a.z;
		face = d.z > 0.0 ? 4 : 5;
		st = d.z > 0.0 ? vec2(d.x, -d.y) : vec2(-d.x, -d.y);
	}
	float size = float(textureSize(map, 0).x);
	// Texel space: texel centres at integers.
	vec2 uv = (st / ma * 0.5 + 0.5) * size - 0.5;
	// Bilinear over the 2x2 block around the sample. Gather order:
	// (i0, j1), (i1, j1), (i1, j0), (i0, j0).
	vec2 i0 = clamp(floor(uv), vec2(0.0), vec2(size - 2.0));
	vec2 f = clamp(uv - i0, 0.0, 1.0);
	vec4 g = textureGather(map, planetMapFaceDir(face, ((i0 + 1.0) / size) * 2.0 - 1.0), 0);
	float lodW = smoothstep(0.5, 1.0, lod);
	if (min(min(g.x, g.y), min(g.z, g.w)) > band.y || max(max(g.x, g.y), max(g.z, g.w)) < band.x)
		return mix(mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y), hw, lodW);
	// Catmull-Rom: the 4x4 block's first texel is o; the sample lies
	// between texels o + 1 and o + 2.
	vec2 o = clamp(floor(uv) - 1.0, vec2(0.0), vec2(size - 4.0));
	vec2 t = clamp(uv - (o + 1.0), 0.0, 1.0);
	// Catmull-Rom weights for texels o .. o + 3, per axis.
	vec2 w0 = t * (-0.5 + t * (1.0 - 0.5 * t));
	vec2 w1 = 1.0 + t * t * (-2.5 + 1.5 * t);
	vec2 w2 = t * (0.5 + t * (2.0 - 1.5 * t));
	vec2 w3 = t * t * (-0.5 + 0.5 * t);
	// Four 2x2 gathers, each at the point midway between its texels (o + 1
	// and o + 3 in texel units -> face coordinates).
	vec2 m0 = ((o + 1.0) / size) * 2.0 - 1.0;
	vec2 m1 = ((o + 3.0) / size) * 2.0 - 1.0;
	vec4 g00 = textureGather(map, planetMapFaceDir(face, vec2(m0.x, m0.y)), 0);
	vec4 g10 = textureGather(map, planetMapFaceDir(face, vec2(m1.x, m0.y)), 0);
	vec4 g01 = textureGather(map, planetMapFaceDir(face, vec2(m0.x, m1.y)), 0);
	vec4 g11 = textureGather(map, planetMapFaceDir(face, vec2(m1.x, m1.y)), 0);
	// Rows j = o.y .. o.y + 3, each filtered along x.
	float r0 = w0.x * g00.w + w1.x * g00.z + w2.x * g10.w + w3.x * g10.z;
	float r1 = w0.x * g00.x + w1.x * g00.y + w2.x * g10.x + w3.x * g10.y;
	float r2 = w0.x * g01.w + w1.x * g01.z + w2.x * g11.w + w3.x * g11.z;
	float r3 = w0.x * g01.x + w1.x * g01.y + w2.x * g11.x + w3.x * g11.y;
	float hf = w0.y * r0 + w1.y * r1 + w2.y * r2 + w3.y * r3;
	return mix(hf, hw, lodW);
}
