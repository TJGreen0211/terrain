// Air density relative to sea level, shared by every shader that marches
// the atmosphere shell (atmosphere.frag, quad.frag, water.frag) and mirrored
// on the CPU for the zenith optical depth (atmoDensity in atmosphere.c).
// Needs fInnerRadius, fOuterRadius and fScaleHeight declared first.
//
// An exponential (one e-fold per fScaleHeight) shifted down so it reaches
// exactly zero at the top of the shell, rescaled to stay 1 at sea level.
// A plain exponential cut off at the top left air there (e^-4 of sea level
// with the default scale height), and rays grazing the top crossed hundreds
// of km of it, so the glow ended in a hard edge. Clamped at the ground: a
// sun ray cutting through the planet sees sea-level density along its
// chord instead of an exploding one.
float atmoDensity(float h)
{
	float top = exp(-(fOuterRadius - fInnerRadius) / fScaleHeight);
	return max(exp(-max(h, 0.0) / fScaleHeight) - top, 0.0) / (1.0 - top);
}
