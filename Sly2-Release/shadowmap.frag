#version 430 core
// PC character shadow maps (render.cpp ApplyShadowMaps; display.h shadowMaps): every pixel of the frame, its world
// position from the scene's depth, looked up in each caster's tile of the depth atlas (the caster's own meshes seen
// from its shadow's light, the original's ShadowOfLightContext centre); behind a caster it is in shadow, filtered over
// 4 x 4 hardware-compared taps (soft edges). The darkness is the original's for that pixel: the kind-7 ramp of the
// combine (shadowcombine.frag) at its frame-alpha level, and the DATE as the volumes': no shadow where the alpha's bit
// 7 is set (the characters' own pixels). Only the colour is drawn (blend 0 x Cs + (1 - As) x Cd).

uniform sampler2D texAlpha;        // unit 1: the frame's alpha at the phase's start (0x16DA48's copy)
uniform sampler2D texDepth;        // unit 3: the scene's depth (reversed Z, [0, 1])
uniform sampler2DShadow texSm;     // unit 4: the atlas, 2 x 2 tiles (standard depth, LEQUAL compare)
uniform mat4 matClipToWorld;       // the pass's world-to-clip, inverted
uniform mat4 amatLight[4];         // per caster: world to its light's clip
uniform vec4 acaster[4];           // per caster: x the lowest z it shades (fading over y above it), z the far fade start
uniform int ccaster;
uniform vec2 viewSize;             // the phase's frame
uniform float rcpAtlas;            // one texel of the atlas
uniform float gain;                // the kind-7 ramp's gain / 255 (128 / 255)
out vec4 FragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int a = int(round(texelFetch(texAlpha, p, 0).a * 128.0));
    if (a >= 128) discard;   // (DATE: the casters' own pixels)
    int level = max((a & 0x7E) - 1, 0);
    float dark = floor(float(level) * 0.007936508 * (gain * 128.0)) / 128.0;
    if (dark <= 0.0) discard;
    float d = texelFetch(texDepth, p, 0).r;
    if (d <= 0.0) discard;
    vec4 wp = matClipToWorld * vec4((vec2(p) + 0.5) / viewSize * 2.0 - 1.0, d, 1.0);
    wp /= wp.w;
    float sh = 0.0;
    for (int i = 0; i < ccaster && i < 4; ++i)
    {
        vec4 c = acaster[i];
        float fLow = smoothstep(c.x, c.x + c.y, wp.z);   // (not through the floor onto the level below)
        if (fLow <= 0.0) continue;
        vec4 lc = amatLight[i] * vec4(wp.xyz, 1.0);
        if (lc.w <= 0.0) continue;
        vec3 l = lc.xyz / lc.w;
        float edge = max(abs(l.x), abs(l.y));
        if (edge >= 1.0) continue;
        vec2 tile = vec2(float(i & 1), float(i >> 1)) * 0.5;
        vec2 uv = tile + (l.xy * 0.5 + 0.5) * 0.5;
        // (inside the tile, a texel and a half from its border, so the taps stay in it)
        uv = clamp(uv, tile + 1.5 * rcpAtlas, tile + 0.5 - 1.5 * rcpAtlas);
        float ref = min(l.z, 1.0) - 0.0005;
        float lit = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                lit += texture(texSm, vec3(uv + (vec2(x, y) - 1.5) * rcpAtlas, ref));
        float s = (1.0 - lit / 16.0) * fLow * (1.0 - smoothstep(0.85, 1.0, edge)) * (1.0 - smoothstep(c.z, 1.0, l.z));
        sh = max(sh, s);
    }
    if (sh <= 0.0) discard;
    FragColor = vec4(0.0, 0.0, 0.0, dark * sh);
}
