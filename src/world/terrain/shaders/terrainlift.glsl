// Geometric lift of a quadtree patch point, in body radii above the unit
// sphere. quad.vert and depth.vert share it, so the shadow caster is the
// very mesh the planet pass draws. The includer declares model, heightMap,
// surfaceMap, seaLevel, landRelief, oceanRelief, oceanDepth, lodCameraPos,
// lodK and lodCellK, and includes patch.glsl, detail.glsl and planetmap.glsl
// first.

// Relief as a fraction of the planet radius (model scale): a linear ramp
// from sea level to the field maximum on land, and a squared shelf down to
// oceanRelief at oceanDepth on the seafloor (water.frag's column thickness
// uses the same profile), plus detail noise on land faded by distance.
// It depends only on the point and its distance, so every patch sharing a
// vertex agrees on it. d: direction, vDist: camera distance, q: cube point;
// addr / addrUV: a patch holding the point and its uv there (the detail
// lattice). gradCube: the detail's vertex slope (detail.glsl); detailM: its
// height (m).
float terrainLift(vec3 d, float vDist, vec3 q, ivec4 addr, vec2 addrUV, out vec2 gradCube,
                  out float detailM)
{
	// Vertex texture fetch of the shaping field at the resolution of the
	// patches at this distance (never finer than a pixel footprint either).
	// (The face-corner bias goes on before the clamp to mip 0.)
	float lodCell = log2(max(vDist*lodCellK, 1e-30));
	float lodPix = log2(max(vDist*lodK, 1e-30));
	float lod = clamp(max(lodCell, lodPix) + planetMapLodBias(d), 0.0, planetMapMaxLod(heightMap));
	// Full-precision filtering (planetMapFine): the hardware's 8-bit
	// sub-texel weights make the ~12 km texels a staircase of ~50 m steps,
	// tens of metres high on steep ground (rows of terraces, saw-tooth
	// silhouettes). Catmull-Rom everywhere (the band spans all heights): a
	// continuous slope, so no crease along the texel grid either.
	float h = planetMapFine(heightMap, d, lod, vec2(-1e9, 1e9));

	float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
	float alt01 = max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3);
	float relief = landRelief * alt01 - oceanRelief * s * s;

	// Detail noise on land, in metres -> body radii.
	vec3 mapGrad = h > seaLevel ? textureLod(surfaceMap, d, lod).xyz : vec3(0.0);
	DetailSpec spec = detailSpec(landRelief * alt01 * detailRadius, length(mapGrad), alt01, length(q));
	detailM = detailVertex(addr, addrUV, length(q), vDist * detailPix, spec,
	                       detailCubeGrad(addr.x, q, mapGrad * detailSlopeToRise), gradCube);
	return relief + detailM / detailRadius;
}

// The lift grid vertex uv is drawn at: its own (lift), or on an edge
// facing a coarser neighbour, that neighbour's straight edge there
// (patchStitch), so the two meshes meet and the skirt stays hidden.
float terrainLiftStitched(vec2 uv, float lift)
{
	PatchStitch ps;
	if (!patchStitch(uv, ps))
		return lift;
	vec2 g;
	float dm;
	vec3 dA = patchDir(ps.uvA), dB = patchDir(ps.uvB);
	float a = terrainLift(dA, length(patchSphere(ps.uvA, dA, model) - lodCameraPos),
	                      patchCube(ps.uvA), ps.addr, ps.aA, g, dm);
	float b = terrainLift(dB, length(patchSphere(ps.uvB, dB, model) - lodCameraPos),
	                      patchCube(ps.uvB), ps.addr, ps.aB, g, dm);
	return mix(a, b, ps.t);
}
