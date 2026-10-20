#version 410

in vec2 TexCoord;

out vec4 FragColor;

uniform sampler2D uTex;
uniform float uHasTex;  // 1 = sample the icon, 0 = flat slot color
uniform vec4 uColor;    // slot colour (rgb + alpha) when uHasTex == 0
uniform float uTint;    // brightness boost, 0 = none
uniform float uAlpha;   // global opacity multiplier
/* Text: the font atlas is white, so a uTintTex of 1 multiplies the sampled
   colour by uColor.rgb to tint it (icons leave it 0 = identity). */
uniform float uTintTex;

void main()
{
    vec4 col = mix(uColor, texture(uTex, TexCoord), uHasTex);
    col.rgb *= (1.0 + uTint) * mix(vec3(1.0), uColor.rgb, uTintTex);
    col.a *= uAlpha;
    FragColor = col;
}
