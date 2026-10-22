#version 410

in vec3 vPosition;

out vec2 fragNdc;

void main()
{
    fragNdc = vPosition.xy;
    gl_Position = vec4(vPosition.xy, 0.0, 1.0);
}
