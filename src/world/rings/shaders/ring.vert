#version 410

// Ring sheet proxy (rings.c): one quad in the ring plane covering the outer
// radius, built from gl_VertexID. It only marks where the view ray meets the
// plane, so the rasterized depth is the exact ray-plane hit; ring.frag does
// all of the ring itself from the interpolated hit position.
uniform mat4 view;
uniform mat4 projection;
uniform vec3 ringCenter;   // camera-relative planet centre
uniform vec3 ringU;
uniform vec3 ringV;
uniform vec2 ringRadii;    // inner, outer (world units)

out vec3 fragPos;

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main()
{
	vec2 c = corners[gl_VertexID] * ringRadii.y * 1.02;
	fragPos = ringCenter + ringU * c.x + ringV * c.y;
	gl_Position = vec4(fragPos, 1.0) * view * projection;
}
