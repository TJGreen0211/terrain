#version 410

// GPU port of generateWaves' packing loop: the transformed spectra, scaled
// to sphere radii (heights) and per patch length (derivatives), written
// straight into the textures water.vert/.frag sample (Water.h layouts).
uniform sampler2D srcA;      // h + i*dDx/dz,  Dx + i*Dz
uniform sampler2D srcB;      // dh/dx + i*dh/dz,  dDx/dx + i*dDz/dz
uniform float scale;         // waves.heightScale
uniform float dscale;        // heightScale * PATCH_SIZE

layout(location = 0) out float outHeight;   // dyTexture
layout(location = 1) out vec2 outDisp;      // dispTexture
layout(location = 2) out vec4 outSlope;     // slopeTexture: sx, sz, sx^2, sz^2
layout(location = 3) out vec4 outJacob;     // jacobTexture

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	vec4 a = texelFetch(srcA, p, 0);
	vec4 b = texelFetch(srcB, p, 0);
	outHeight = a.x * scale;
	outDisp = a.zw * scale;
	vec2 s = b.xy * dscale;
	outSlope = vec4(s, s * s);
	outJacob = vec4(b.zw * dscale, a.y * dscale, 0.0);
}
