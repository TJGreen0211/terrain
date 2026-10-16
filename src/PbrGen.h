#ifndef PBRGEN_H
#define PBRGEN_H

// Renders the planet's PBR maps (albedo, height, normal, roughness, metallic, AO)
// from the planet's noise field (the same implementation the planet shader uses)
// into a per-face FBO, then writes each cube face out as its own set of PNGs
// under ../assets/planet/pbr/<face>/.
void generatePlanetPbrTextures(void);

#endif
