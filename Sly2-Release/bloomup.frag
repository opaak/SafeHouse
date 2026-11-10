#version 430 core
// Bloom (post.cpp): one step up the chain, a 3x3 tent over the smaller level, added (blend ONE, ONE) into the next
// larger level, so each level's glow spreads wider than the one above it.

uniform sampler2D tex;   // the smaller level
uniform vec2 rcpSize;    // one texel of it
in vec2 vUv;
out vec4 FragColor;

vec3 S(float x, float y) { return texture(tex, vUv + vec2(x, y) * rcpSize).rgb; }

void main()
{
    vec3 col = S(-1.0, 1.0) + 2.0 * S(0.0, 1.0) + S(1.0, 1.0)
             + 2.0 * S(-1.0, 0.0) + 4.0 * S(0.0, 0.0) + 2.0 * S(1.0, 0.0)
             + S(-1.0, -1.0) + 2.0 * S(0.0, -1.0) + S(1.0, -1.0);
    FragColor = vec4(col / 16.0, 1.0);
}
