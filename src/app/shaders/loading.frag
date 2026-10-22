#version 410

in vec2 fragNdc;

out vec4 FragColor;

uniform float uProgress;
uniform float uTime;

void main()
{
    vec3 col = vec3(0.02, 0.02, 0.05);

    // Progress bar centred low in the window: 60% of screen width, thin strip.
    vec2 barMin = vec2(-0.30, -0.050);
    vec2 barMax = vec2( 0.30, -0.020);

    if (fragNdc.x >= barMin.x && fragNdc.x <= barMax.x &&
        fragNdc.y >= barMin.y && fragNdc.y <= barMax.y)
    {
        float w = barMax.x - barMin.x;
        col = vec3(0.12, 0.12, 0.18); // track
        if (fragNdc.x - barMin.x <= uProgress * w)
        {
            // Subtle pulse so the bar reads as active while a decode blocks.
            float pulse = 0.85 + 0.15 * sin(uTime * 4.0);
            col = vec3(0.30, 0.60, 1.0) * pulse; // fill
        }
    }

    FragColor = vec4(col, 1.0);
}
