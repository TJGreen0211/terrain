#version 410

// Camera-facing quad around a body's silhouette (planets.c), or the whole
// screen up close. All in view space: the camera is the origin, the view is
// the main camera's.

in vec2 vCorner;            // unit quad corner in [-1,1]^2

uniform mat4 projection;    // main camera (row layout: v * projection)
uniform vec3 centerView;    // body centre, view space
uniform float halfSize;     // quad half-size in the plane through the centre
uniform int fullScreen;     // 1: the whole screen (the camera is close)

void main()
{
	if (fullScreen == 1) {
		gl_Position = vec4(vCorner, 0.0, 1.0);
		return;
	}
	// Plane through the centre, facing the camera position (not the view
	// axis): the silhouette cone cuts it in a circle of radius halfSize.
	vec3 dir = normalize(centerView);
	vec3 ref = abs(dir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 u = normalize(cross(dir, ref));
	vec3 v = cross(u, dir);
	vec3 p = centerView + halfSize * (vCorner.x * u + vCorner.y * v);
	gl_Position = vec4(p, 1.0) * projection;
}
