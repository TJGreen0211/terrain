#version 410

// The cloud slab's coverage (clouds.c): a camera-facing quad around the
// layer's silhouette, as the stand-ins use (planet_impostor.vert), or the
// whole screen when the camera is near or inside the layer. The fragment
// shader ray-traces the layer and writes its own depth. Drawn as a 4-vertex
// fan with no vertex buffer.

uniform mat4 projection;    // main camera (row layout: v * projection)
uniform vec3 centerView;    // planet centre, view space
uniform float halfSize;     // quad half-size in the plane through the centre
uniform int fullScreen;

void main()
{
	int id = gl_VertexID;
	vec2 corner = vec2(id == 1 || id == 2 ? 1.0 : -1.0, id >= 2 ? 1.0 : -1.0);
	if (fullScreen == 1) {
		gl_Position = vec4(corner, 0.0, 1.0);
		return;
	}
	vec3 dir = normalize(centerView);
	vec3 ref = abs(dir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 u = normalize(cross(dir, ref));
	vec3 v = cross(u, dir);
	vec3 p = centerView + halfSize * (corner.x * u + corner.y * v);
	gl_Position = vec4(p, 1.0) * projection;
}
