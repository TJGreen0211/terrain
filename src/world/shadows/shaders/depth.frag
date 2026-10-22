#version 410

// Depth-only: the rasterized light-space depth is the shadow map value.
// Not writing gl_FragDepth keeps early-Z and the polygon offset working.
void main()
{
}
