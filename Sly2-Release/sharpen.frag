#version 430 core
// Contrast-adaptive sharpening of the 3D picture (post.cpp; display.h upscaler Sharp, sharpness), after AMD's CAS
// (FidelityFX, MIT): a 3x3 cross filter whose negative lobe shrinks where the neighbourhood already has contrast, so
// soft detail is sharpened while hard edges (the cel borders, the HUD-like signs) do not ring. Alpha passes through.

uniform sampler2D tex;    // the picture
uniform float sharpness;  // 0 .. 1
in vec2 vUv;
out vec4 FragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = textureSize(tex, 0) - 1;
    vec3 a = texelFetch(tex, clamp(p + ivec2(-1, 1), ivec2(0), lim), 0).rgb;
    vec3 b = texelFetch(tex, clamp(p + ivec2(0, 1), ivec2(0), lim), 0).rgb;
    vec3 c = texelFetch(tex, clamp(p + ivec2(1, 1), ivec2(0), lim), 0).rgb;
    vec3 d = texelFetch(tex, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).rgb;
    vec4 e4 = texelFetch(tex, clamp(p, ivec2(0), lim), 0);
    vec3 e = e4.rgb;
    vec3 f = texelFetch(tex, clamp(p + ivec2(1, 0), ivec2(0), lim), 0).rgb;
    vec3 g = texelFetch(tex, clamp(p + ivec2(-1, -1), ivec2(0), lim), 0).rgb;
    vec3 h = texelFetch(tex, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).rgb;
    vec3 i = texelFetch(tex, clamp(p + ivec2(1, -1), ivec2(0), lim), 0).rgb;

    // Soft minimum and maximum of the cross and of the whole 3x3 (each doubled once: the sum of both).
    vec3 mnCross = min(min(min(d, e), min(f, b)), h);
    vec3 mn = mnCross + min(mnCross, min(min(a, c), min(g, i)));
    vec3 mxCross = max(max(max(d, e), max(f, b)), h);
    vec3 mx = mxCross + max(mxCross, max(max(a, c), max(g, i)));

    // How far the neighbourhood is from clipping at either end: the room the filter has (0: none).
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 1024.0)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, clamp(sharpness, 0.0, 1.0));
    vec3 w = amp * peak;
    vec3 rgb = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    FragColor = vec4(clamp(rgb, 0.0, 1.0), e4.a);
}
