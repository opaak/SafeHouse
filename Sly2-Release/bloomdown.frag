#version 430 core
// Bloom (post.cpp; display.h bloom): one step down the chain, half the size of its source, with the 13-tap filter
// (Jimenez, "Next generation post processing in Call of Duty: Advanced Warfare", 2014), which does not flicker as the
// camera moves the way a plain 2x2 average does. The first step (fPrefilter) keeps only what is brighter than the
// threshold, with a soft knee, so ordinary surfaces do not glow; the lights, windows and neon do.

uniform sampler2D tex;      // the source level (the resolved picture for the first step)
uniform vec2 rcpSize;       // one source texel
uniform int fPrefilter;
uniform float threshold;    // 0..1, of the brightest channel
uniform float knee;
in vec2 vUv;
out vec4 FragColor;

vec3 S(float x, float y) { return texture(tex, vUv + vec2(x, y) * rcpSize).rgb; }

void main()
{
    vec3 a = S(-2.0, 2.0), b = S(0.0, 2.0), c = S(2.0, 2.0);
    vec3 d = S(-2.0, 0.0), e = S(0.0, 0.0), f = S(2.0, 0.0);
    vec3 g = S(-2.0, -2.0), h = S(0.0, -2.0), i = S(2.0, -2.0);
    vec3 j = S(-1.0, 1.0), k = S(1.0, 1.0), l = S(-1.0, -1.0), m = S(1.0, -1.0);
    vec3 col = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    if (fPrefilter != 0)
    {
        float br = max(col.r, max(col.g, col.b));
        float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
        rq = rq * rq / (4.0 * knee + 1e-4);
        col *= max(rq, br - threshold) / max(br, 1e-4);
    }
    FragColor = vec4(col, 1.0);
}
