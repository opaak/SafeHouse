#version 430 core
// Ambient occlusion (post.cpp; display.h ambientOcclusion), its second pass: ao.frag's openness blurred over 5 x 5
// pixels (weighted by depth, so it does not bleed across silhouettes), turned into two soft steps of darkness, as the
// cel shading's own two tones, and multiplied into the picture. Alpha passes through.

uniform sampler2D tex;        // the picture
uniform sampler2D texAo;      // unit 1: ao.frag's openness (r)
uniform sampler2D texDepth;   // unit 2: the scene's depth
uniform sampler2D texMask;    // unit 3: the characters' own pixels (render.cpp FBuildCharacterMask)
uniform int fMask;
uniform float sNear;
uniform float strength;       // 0..1
in vec2 vUv;
out vec4 FragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = textureSize(tex, 0) - 1;
    vec4 c = texelFetch(tex, p, 0);
    float d0 = texelFetch(texDepth, p, 0).r;
    // (the characters' own pixels: their low-poly creases would step into dark triangles; they have their own cel shading)
    if (d0 <= 0.0 || (fMask != 0 && texelFetch(texMask, p, 0).r > 0.5))
    {
        FragColor = c;
        return;
    }
    float w0 = sNear / d0;
    float sum = 0.0, wsum = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), lim);
            float d = texelFetch(texDepth, q, 0).r;
            if (d <= 0.0) continue;
            float wt = exp(-abs(sNear / d - w0) / (0.04 * w0));
            sum += texelFetch(texAo, q, 0).r * wt;
            wsum += wt;
        }
    float occ = 1.0 - (wsum > 0.0 ? sum / wsum : 1.0);
    // two steps (half and full), each with a narrow soft edge
    float tone = 0.5 * smoothstep(0.16, 0.24, occ) + 0.5 * smoothstep(0.42, 0.50, occ);
    FragColor = vec4(c.rgb * (1.0 - strength * 0.5 * tone), c.a);
}
