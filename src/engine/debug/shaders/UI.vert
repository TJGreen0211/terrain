#version 410

in vec3 vPosition;   // unit quad, [-1, 1] both axes, y up
in vec2 texCoord;

out vec2 TexCoord;

/* Button rect in window content pixels, y-down from the top-left — the
   same space glfwGetCursorPos reports, so hit testing is a direct
   comparison. uViewport is the GL viewport these NDCs scale into (window
   size; on HiDPI the framebuffer, a uniform scale of window space). */
uniform vec4 uRect;
uniform vec2 uViewport;
/* Icon draws use the full texture (uUVOff 0, uUVScale 1). Text glyphs set
   these to the sub-rect of one cell in the shared bitmap-font atlas. */
uniform vec2 uUVOff;
uniform vec2 uUVScale;

void main()
{
    vec2 px = uRect.xy
        + vec2(vPosition.x * 0.5 + 0.5, 0.5 - vPosition.y * 0.5) * uRect.zw;
    vec2 ndc = vec2(px.x * 2.0 / uViewport.x - 1.0,
                    1.0 - px.y * 2.0 / uViewport.y);
    gl_Position = vec4(ndc, 0.0, 1.0);
    TexCoord = uUVOff + texCoord * uUVScale;
}
