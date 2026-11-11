#version 430 core
// PC soft shadows (display.h softShadows; render.cpp ShadowCombine): the combine's darkening (shadowcombine.frag's
// ramps, the same CLUT levels and gains) averaged over a disc of radius pixels, so the volumes' hard, jagged edges
// fade over a few pixels. The marks come from a single-sample copy of the stencil (resolved: one sample per pixel), the
// depth from the same copy; a tap counts only where its depth is close to the pixel's (the shadow does not bleed onto
// what stands in front of or far behind it). Only the colour is drawn here (blend 0 x Cs + (1 - As) x Cd); the frame's
// alpha is still set by the original ramps afterward.

uniform sampler2D texAlpha;     // unit 1: the frame's alpha before the marking (shadowcombine.frag)
uniform usampler2D texStencil;  // unit 2: the marks
uniform sampler2D texDepth;     // unit 3: the depth (reversed Z)
uniform uint maskMarks;         // the stencil bits the ramps compare (STENCIL_SH7, | STENCIL_SH8 with kind 8)
uniform uint stencil7;          // STENCIL_SH7
uniform uint stencil8;          // STENCIL_SH8
uniform vec3 gain;              // the three ramps' gains / 255 (kind 7, kind 8, both)
uniform ivec3 framp;            // which ramps this phase drew
uniform float radius;           // pixels
out vec4 FragColor;

ivec2 s_lim;

float Dark(ivec2 q)
{
    uint s = texelFetch(texStencil, q, 0).r & maskMarks;
    int iramp = s == stencil7 ? 0 : (s == stencil8 ? 1 : (s == (stencil7 | stencil8) ? 2 : -1));
    if (iramp < 0 || framp[iramp] == 0) return 0.0;
    int a = int(round(texelFetch(texAlpha, q, 0).a * 128.0));
    int level = max((a & 0x7E) + (iramp == 0 ? 0 : 1) - 1, 0);
    return floor(float(level) * 0.007936508 * (gain[iramp] * 128.0)) / 128.0;
}

void main()
{
    s_lim = textureSize(texStencil, 0) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d0 = texelFetch(texDepth, p, 0).r;
    const vec2 adisc[12] = vec2[12](
        vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
        vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
        vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
    float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    mat2 rot = mat2(cos(ang), sin(ang), -sin(ang), cos(ang));
    float sum = Dark(p), wsum = 1.0;
    for (int i = 0; i < 12; ++i)
    {
        ivec2 q = clamp(p + ivec2(round(rot * adisc[i] * radius)), ivec2(0), s_lim);
        float d = texelFetch(texDepth, q, 0).r;
        // (reversed Z: the ratio of the depths is the inverse ratio of the distances)
        float wt = d0 > 0.0 && d > 0.0 ? exp(-abs(d / d0 - 1.0) / 0.04) : (d0 <= 0.0 && d <= 0.0 ? 1.0 : 0.0);
        sum += Dark(q) * wt;
        wsum += wt;
    }
    FragColor = vec4(0.0, 0.0, 0.0, sum / wsum);
}
