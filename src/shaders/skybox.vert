#version 410

in vec4 vPosition;
out vec3 TexCoords;

uniform mat4 ModelView;
uniform mat4 projection;

void main()
{
	// Row-vector pipeline (matches the rest of this codebase). pos.xyww pushes
	// the box to the far clipping plane (z/w == 1) so it always sits behind
	// the scene; its NDC x/y are unchanged, so the box still fills the screen.
	vec4 pos = vPosition*ModelView*projection;
	gl_Position = pos.xyww;
	TexCoords = vPosition.xyz;
}
