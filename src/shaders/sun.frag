#version 410

in vec2 fragNdc;
in vec2 sunNdc;
in float sunHalf;
in float sunDepth;
out vec4 FragColor;

uniform float systemTime;
uniform float aspect;
uniform sampler2D tex;
uniform sampler2D normalTex;
uniform float uDisc;

float snoise(vec3 uv, float res)	// by trisomie21
{
	const vec3 s = vec3(1e0, 1e2, 1e4);

	uv *= res;

	vec3 uv0 = floor(mod(uv, res))*s;
	vec3 uv1 = floor(mod(uv+vec3(1.), res))*s;

	vec3 f = fract(uv); f = f*f*(3.0-2.0*f);

	vec4 v = vec4(uv0.x+uv0.y+uv0.z, uv1.x+uv0.y+uv0.z,
		      	  uv0.x+uv1.y+uv0.z, uv1.x+uv1.y+uv0.z);

	vec4 r = fract(sin(v*1e-3)*1e5);
	float r0 = mix(mix(r.x, r.y, f.x), mix(r.z, r.w, f.x), f.y);

	r = fract(sin((v + uv1.z - uv0.z)*1e-3)*1e5);
	float r1 = mix(mix(r.x, r.y, f.x), mix(r.z, r.w, f.x), f.y);

	return mix(r0, r1, f.z)*2.-1.;
}

void main()
{
	// Position of this pixel relative to the sun's centre, in the same units
	// the corona math expects: 0 at the centre, ~2 at the corona's full
	// extent, which spans sunHalf NDC units on each side of the centre.
	vec2 starOffset = ( fragNdc - sunNdc ) * ( 2.0 / max( sunHalf, 1e-4 ) );

	// Early-out before the 16-octave noise: it only matters near the sun.
	// disc pass: radius is always <= 0.34 (0.24 + brightness*0.2,
	//            brightness <= 0.5) -> 0.35 is a safe tight bound.
	// corona pass: the halo term is exactly 0 at dist >= ~0.61, starGlow
	//              is 0 at dist >= 2, and the f-term residual beyond
	//              dist 2 is < 1% of orange -> 2.0 loses nothing visible.
	vec2 earlyP = vec2( starOffset.x * aspect * 0.5, starOffset.y * 0.5 );
	if ( uDisc > 0.5 ) { if ( length( earlyP ) >= 0.35 ) discard; }
	else               { if ( length( earlyP ) > 2.0 )    discard; }

	float freqs[4];
	freqs[0] = texture( normalTex, vec2( 0.01, 0.25 ) ).x;
	freqs[1] = texture( normalTex, vec2( 0.07, 0.25 ) ).x;
	freqs[2] = texture( normalTex, vec2( 0.15, 0.25 ) ).x;
	freqs[3] = texture( normalTex, vec2( 0.30, 0.25 ) ).x;

	float brightness	= freqs[1] * 0.25 + freqs[2] * 0.25;
	float radius		= 0.24 + brightness * 0.2;
	float invRadius 	= 1.0/radius;

	vec3 orange			= vec3( 0.8, 0.65, 0.3 );
	vec3 orangeRed		= vec3( 0.8, 0.35, 0.1 );
	float time		= systemTime * 0.1;

	vec2 p 			= vec2( starOffset.x * aspect * 0.5, starOffset.y * 0.5 );

	float fade		= pow( length( 2.0 * p ), 0.5 );
	float fVal1		= 1.0 - fade;
	float fVal2		= 1.0 - fade;

	float angle		= atan( p.x, p.y )/6.2832;
	float dist		= length(p);
	vec3 coord		= vec3( angle, dist, time * 0.1 );

	float newTime1	= abs( snoise( coord + vec3( 0.0, -time * ( 0.35 + brightness * 0.001 ), time * 0.015 ), 15.0 ) );
	float newTime2	= abs( snoise( coord + vec3( 0.0, -time * ( 0.15 + brightness * 0.001 ), time * 0.015 ), 45.0 ) );
	for( int i=1; i<=7; i++ ){
		float power = pow( 2.0, float(i + 1) );
		fVal1 += ( 0.5 / power ) * snoise( coord + vec3( 0.0, -time, time * 0.2 ), ( power * ( 10.0 ) * ( newTime1 + 1.0 ) ) );
		fVal2 += ( 0.5 / power ) * snoise( coord + vec3( 0.0, -time, time * 0.2 ), ( power * ( 25.0 ) * ( newTime2 + 1.0 ) ) );
	}

	float corona		= pow( fVal1 * max( 1.1 - fade, 0.0 ), 2.0 ) * 50.0;
	corona				+= pow( fVal2 * max( 1.1 - fade, 0.0 ), 2.0 ) * 50.0;
	corona				*= 1.2 - newTime1;
	vec3 starSphere		= vec3( 0.0 );

	vec2 sp = vec2( starOffset.x * aspect, starOffset.y );
	sp *= ( 2.0 - brightness );
	float r = max(dot(sp,sp), 1e-4);
	float f = (1.0-sqrt(abs(1.0-r)))/(r) + brightness * 0.5;
	if( dist < radius ){
		corona			*= pow( dist * invRadius, 24.0 );
		vec2 newUv;
		newUv.x = sp.x*f;
		newUv.y = sp.y*f;
		newUv += vec2( time, 0.0 );

		vec3 texSample 	= texture( tex, newUv ).rgb;
		float uOff		= ( texSample.g * brightness * 4.5 + time );
		vec2 starUV		= newUv + vec2( uOff, 0.0 );
		starSphere		= texture( tex, starUV ).rgb;
	}

	float starGlow	= min( max( 1.0 - dist * ( 1.0 - brightness ), 0.0 ), 1.0 );

	// Two passes over the same billboard, selected by uDisc:
	//  uDisc = 1 : the opaque sphere (central disc). Kept where dist < radius
	//              and written with the sun's real depth, so it occludes any
	//              planet that orbits behind it.
	//  uDisc = 0 : the corona halo (outside the disc). Additive and never
	//              writes depth, so planets orbiting behind the halo still show.
	if( uDisc > 0.5 )
	{
		if( dist >= radius )
			discard;

		vec3 core = vec3( f * ( 0.75 + brightness * 0.3 ) * orange )
		 		 + starSphere
		 		 + corona * orange
		 		 + starGlow * orangeRed;
		FragColor	= vec4( clamp( core, 0.0, 1.0 ), 1.0 );
		gl_FragDepth = sunDepth;
	}
	else
	{
		if( dist < radius )
			discard;

		FragColor.rgb	= vec3( f * ( 0.75 + brightness * 0.3 ) * orange )
		 			 + corona * orange
		 			 + starGlow * orangeRed;
		FragColor.a		= 1.0;
		// Test-only depth so the halo is still hidden by geometry in front;
		// the depth mask is off on the CPU side, so nothing is written.
		gl_FragDepth = sunDepth;
	}
}
