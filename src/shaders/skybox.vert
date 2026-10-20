#version 410

in vec4 vPosition;
out vec3 TexCoords;

uniform mat4 ModelView;
uniform mat4 projection;

void main()
{
	// Row-vector pipeline (matches the rest of this codebase). z ~ 0 pushes
	// the box to the far plane of the reversed-Z main pass (depth 0) so it
	// always sits behind the scene; its NDC x/y are unchanged, so the box
	// still fills the screen. A hair above 0 so the [0, w] clip range never
	// rounds it away.
	vec4 pos = vPosition*ModelView*projection;
	gl_Position = vec4(pos.xy, pos.w * 1e-6, pos.w);
	TexCoords = vPosition.xyz;
}
