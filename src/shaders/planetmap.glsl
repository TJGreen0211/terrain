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
// lod 1 this blends the base level's 2x2 texels by hand (textureGather,
// weights from the face coordinate, GL's cube face table); from lod 1 up
// the steps are below a pixel and the hardware filter stands. quad.frag
// and water.frag both read the height through this, so the land and the
// water discard keep meeting on the same contour.
//
// The 2x2 block is chosen here and gathered at the exact point between its
// texels: gathering at d itself let the hardware's fixed-point coordinate
// pick a different block than floor() along texel rows and columns, which
// drew the texel grid as thin dark lines. Blocks stay inside the face (the
// weights extrapolate the last half texel at a face edge).
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

float planetMapFine(samplerCube map, vec3 d, float lod)
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
	vec2 i0 = clamp(floor(uv), vec2(0.0), vec2(size - 2.0));
	vec2 f = clamp(uv - i0, 0.0, 1.0);
	// The point midway between texels i0 and i0 + 1, back on the face.
	vec2 mid = ((i0 + 1.0) / size) * 2.0 - 1.0;
	// Gather order: (i0, j1), (i1, j1), (i1, j0), (i0, j0).
	vec4 g = textureGather(map, planetMapFaceDir(face, mid), 0);
	float hf = mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y);
	return mix(hf, hw, smoothstep(0.5, 1.0, lod));
}
