// Water column optics, shared by water.frag (the surface's opacity) and
// quad.frag (the seafloor seen through it), so both agree on how much of
// the floor shows. dT: depth below sea level / oceanDepth (0 at the coast).

// Light absorbed per planet radius of optical path, per channel. Red goes
// first, then green, so sand in the shallows turns turquoise and the deep
// sea navy. The water surface's alpha carries the blue (least absorbed)
// channel; quad.frag tints the floor by the extra red/green loss. Set for
// the look, not real water: 2900 kept the sea's colours when the seafloor
// went from 64 km to Earth's real 11 km (it was 500 against 64 km).
const float WATER_ABSORB = 2900.0;
const vec3 WATER_ABSORB_RGB = WATER_ABSORB * vec3(6.0, 2.2, 1.0);
// Optical path: the geometric column (oceanRelief dT^2, the drawn seafloor
// profile) plus a shelf term linear in dT. The squared profile alone keeps
// the whole shelf nearly clear, which reads as a milky band of bare sand
// along every coast; the linear term tints the shallows within a short
// distance of the shore while still reaching 1 (clear) at the coastline.
const float WATER_SHELF_TURBIDITY = 0.25;

float waterOpticalDepth(float dT, float relief)
{
	return relief * dT * (dT + WATER_SHELF_TURBIDITY);
}

// Scalar (blue) transmittance down to the floor and back along a view
// ray that meets the surface at grazing cosine graz.
float waterTransmit(float dT, float relief, float graz)
{
	return exp(-WATER_ABSORB * waterOpticalDepth(dT, relief) / graz);
}

// Extra per-channel transmittance on top of waterTransmit (1 in blue).
vec3 waterFloorTint(float dT, float relief, float graz)
{
	return exp(-(WATER_ABSORB_RGB - WATER_ABSORB) * waterOpticalDepth(dT, relief) / graz);
}
