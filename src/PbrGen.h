#ifndef PBRGEN_H
#define PBRGEN_H

// Renders the planet's PBR maps (albedo, height, normal, roughness, metallic, AO)
// from the planet's noise field (the same implementation the planet shader uses)
// into a per-face FBO, then writes each cube face out as its own set of PNGs
// under ../assets/planet/pbr/<face>/.
void generatePlanetPbrTextures(void);

// Bakes the planet height field (quad.frag's fbm3) into a 2048x1024
// equirectangular GL_R16F texture with a full mipmap chain and returns its
// texture id. quad.frag/water.frag sample this instead of evaluating simplex
// noise per pixel (heightMap uniform, lodK = lod scale, lodCameraPos).
unsigned int bakePlanetHeightmap(void);

#endif
