#version 410

in vec3 vPosition;   // fullscreen-quad corner in NDC [-1,1]

out vec2 fragNdc;    // per-fragment NDC coordinate of the pixel
out vec2 sunNdc;     // sun centre in NDC
out float sunHalf;   // projected NDC half-size of the sun corona
out float sunDepth;  // sun centre's depth-buffer value in [0,1]

uniform mat4 view;
uniform mat4 projection;
uniform float sunRadius;
uniform vec3 sunCenter;   // sun centre, camera-relative (camera at the origin)

void main()
{
	// Draw a fullscreen pass (not a sized billboard): the quad always covers
	// the whole viewport, so a fragment is never clipped to a partial triangle
	// when the sun sits off-centre. The sun itself is placed/sized in the
	// fragment shader from its projected NDC centre and half-size.
	fragNdc = vPosition.xy;
	// z mid-range: the depth comes from gl_FragDepth (sunDepth); 0 would sit
	// on the edge of the main pass's [0, w] clip range.
	gl_Position = vec4(vPosition.xy, 0.5, 1.0);

	vec4 sunView = vec4(sunCenter, 1.0) * view;
	if( sunView.z >= 0.0 )
	{
		// At or behind the camera: put the sun far off-screen so nothing draws.
		sunNdc = vec2(1e5);
		sunHalf = 1.0;
		sunDepth = 0.0;   // reversed-Z far plane
		return;
	}

	vec4 clip = sunView * projection;
	vec2 ndcPos = clip.xy / clip.w;

	// Projected NDC half-size of a sphere of radius sunRadius at the sun's
	// depth: project the sun centre and a point sunRadius away in view-space
	// +X (same depth) and take the difference in NDC x.
	vec4 clipR = (sunView + vec4(sunRadius, 0.0, 0.0, 0.0)) * projection;

	sunNdc   = ndcPos;
	sunHalf  = abs( clipR.x / clipR.w - ndcPos.x );
	// Main pass uses a [0,1] clip depth range (reversed-Z), so window depth
	// is the NDC z itself.
	sunDepth = clip.z / clip.w;
}
