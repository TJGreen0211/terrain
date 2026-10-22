// Cube-face mapping of the tiled wave maps (FFT patch, detail normals) onto
// the body-local unit sphere. Each point takes the face of its direction's
// major axis and that face's two minor components as plane coordinates q,
// so there is no pole and the texel density varies by at most ~1.7x. Near
// a cube edge (within WAVE_FACE_BLEND of the major component) the
// neighbouring faces' independent wave fields are blended in, with weights
// that keep the wave contrast through the seam (see waveFaceNorm).
//
// q is in body radii near the face centre, so q * waveTilings[c] (cascade
// c's patches per body radius) is the wave maps' uv in layer c.

// FFT cascades (water.c), largest first: must match OCEAN_CASCADES
// (WorldConfig.h).
const int WAVE_CASCADES = 3;
const float WAVE_FACE_BLEND = 0.03;

// Blend weight of each face (x, y, z major), summing to 1. Every
// component is 0 or 1 except inside the edge bands.
vec3 waveFaceWeights(vec3 d)
{
	vec3 a = abs(d);
	float m = max(a.x, max(a.y, a.z));
	vec3 w = smoothstep(m - WAVE_FACE_BLEND, m, a);
	return w / (w.x + w.y + w.z);
}

// Scale for weighted sums of the faces' (uncorrelated) wave fields that
// keeps their variance: a plain average would calm the sea along the seams.
float waveFaceNorm(vec3 w)
{
	return inversesqrt(dot(w, w));
}

// Face k's minor components of d, in the order the face uses them.
vec3 waveFacePermute(int k, vec3 v)
{
	return k == 0 ? v.yzx : (k == 1 ? v.zxy : v);
}

// Each cascade's map is rotated and shifted on every face: the patches
// are square, so cascades sharing the face axes line their repeats up
// with each other and with the face, which reads as rows of repeats
// running across the sea. The angles stay well apart modulo the square's
// quarter turn (golden-ratio steps), and differ between faces. Map uv =
// R * (q * tiling) + offset; anything the map returns along its own axes
// goes back to the face axes with R's transpose (v * R in GLSL).
mat2 waveCascadeRot(int k, int c)
{
	float a = 1.5707963 * fract(0.6180340 * float(c) + 0.3819660 * float(k) + 0.2);
	float s = sin(a), co = cos(a);
	return mat2(co, s, -s, co);
}

vec2 waveCascadeOffset(int k, int c)
{
	return fract(vec2(0.7548777, 0.5698403) * float(1 + c + 3 * k));
}

// Back from face k's permuted order to body-local xyz.
vec3 waveFaceUnpermute(int k, vec3 v)
{
	return k == 0 ? v.zxy : (k == 1 ? v.yzx : v);
}

// Plane coordinates q of unit direction d on face k, and their tangents
// on the sphere: moving q.x by dq (q.y held, the point kept on the sphere)
// moves the point by E0 * dq. Both are tangent to the sphere at d.
vec2 waveFace(int k, vec3 d, out vec3 E0, out vec3 E1)
{
	vec3 p = waveFacePermute(k, d);
	E0 = waveFaceUnpermute(k, vec3(1.0, 0.0, -p.x / p.z));
	E1 = waveFaceUnpermute(k, vec3(0.0, 1.0, -p.y / p.z));
	return p.xy;
}

// Body-local gradient (tangent to the sphere) of a field whose partial
// derivatives along q are dq. The tangents are not orthonormal away from
// the face centre, so this goes through the inverse metric.
vec3 waveFaceGrad(vec2 dq, vec3 E0, vec3 E1)
{
	float g00 = dot(E0, E0), g01 = dot(E0, E1), g11 = dot(E1, E1);
	vec2 c = vec2(g11 * dq.x - g01 * dq.y, g00 * dq.y - g01 * dq.x) / (g00 * g11 - g01 * g01);
	return c.x * E0 + c.y * E1;
}
