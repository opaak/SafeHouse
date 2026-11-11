#version 430 core
// PC world shadows (TEST; render.cpp DrawWorldShadows, display.h worldShadows): every pixel of the frame, its world
// position from the scene's depth, looked up in the world's shadow map (the level's static meshes seen from its main
// directional light, an orthographic box round the camera's view). Behind a caster it is in shadow: 3 x 3
// hardware-compared taps, then a narrow step (one cel tone, as the shading's own, not a smooth gradient). Surfaces
// already turned away from the light are left as the art painted them (no second darkening), and the shadow fades out
// toward the box's edge. The characters' own pixels (the frame alpha's bit 7) are left alone. Only the colour is drawn (blend 0 x Cs + (1 - As) x Cd); the alpha is kept.

uniform sampler2D texDepth;        // unit 3: the scene's depth (reversed Z, [0, 1]; 0 nothing drawn)
uniform sampler2DShadow texWs;     // unit 4: the shadow map (standard depth, LEQUAL compare)
uniform sampler2D texColor;        // unit 5: the frame's colour (its alpha: bit 7 the characters' own pixels)
uniform mat4 matClipToWorld;       // the frame's world-to-clip, inverted
uniform mat4 matLight;             // world to the map's clip ([-1, 1] across, [0, 1] deep)
uniform vec3 dirLight;             // the light's direction (from the light, normalized)
uniform vec2 viewSize;
uniform float rcpMap;              // one texel of the map
uniform float texelWorld;          // one texel of the map in world units
uniform float strength;            // 0..1: how dark the shadow is (x 0.5)
out vec4 FragColor;

ivec2 s_lim;
vec3 WorldAt(ivec2 p, float d)
{
    vec4 w = matClipToWorld * vec4((vec2(p) + 0.5) / viewSize * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

void main()
{
    s_lim = ivec2(viewSize) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d0 = texelFetch(texDepth, p, 0).r;
    if (d0 <= 0.0) discard;   // (the sky)
    // (the characters: their own shadows and shading; their low-poly facets would take the shadow triangle by triangle)
    if (int(round(texelFetch(texColor, p, 0).a * 128.0)) >= 128) discard;
    vec3 P = WorldAt(p, d0);

    // the surface's normal from the nearer neighbour of each side (no smearing across silhouettes)
    ivec2 pl = max(p - ivec2(1, 0), ivec2(0)), pr = min(p + ivec2(1, 0), s_lim);
    ivec2 pb = max(p - ivec2(0, 1), ivec2(0)), pt = min(p + ivec2(0, 1), s_lim);
    float dl = texelFetch(texDepth, pl, 0).r, dr = texelFetch(texDepth, pr, 0).r;
    float db = texelFetch(texDepth, pb, 0).r, dt = texelFetch(texDepth, pt, 0).r;
    bool fRight = dr > 0.0 && (dl <= 0.0 || abs(dr - d0) < abs(d0 - dl));
    bool fUp = dt > 0.0 && (db <= 0.0 || abs(dt - d0) < abs(d0 - db));
    if ((!fRight && dl <= 0.0) || (!fUp && db <= 0.0)) discard;
    vec3 ddx = fRight ? WorldAt(pr, dr) - P : P - WorldAt(pl, dl);
    vec3 ddy = fUp ? WorldAt(pt, dt) - P : P - WorldAt(pb, db);
    vec3 N = normalize(cross(ddx, ddy));
    vec3 toEye = WorldAt(p, 1.0) - P;   // (toward the camera: the near plane at this pixel)
    if (dot(N, toEye) < 0.0) N = -N;
    float ndl = dot(N, -dirLight);
    float facing = smoothstep(0.02, 0.25, ndl);   // (turned away: already the art's shade)
    if (facing <= 0.0) discard;

    // looked up a little off the surface (along its normal, more where the light grazes it): no self-shadowing
    vec3 Po = P + N * texelWorld * (1.5 + 2.0 * (1.0 - ndl));
    vec4 lc = matLight * vec4(Po, 1.0);
    vec3 l = lc.xyz / lc.w;
    float edge = max(abs(l.x), abs(l.y));
    if (edge >= 1.0 || l.z >= 1.0 || l.z <= 0.0) discard;
    vec2 uv = l.xy * 0.5 + 0.5;
    float ref = l.z - 0.0004;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += texture(texWs, vec3(uv + vec2(x, y) * rcpMap, ref));
    float sh = smoothstep(0.3, 0.7, 1.0 - lit / 9.0);
    sh *= facing * (1.0 - smoothstep(0.8, 1.0, edge));
    if (sh <= 0.0) discard;
    FragColor = vec4(0.0, 0.0, 0.0, strength * 0.5 * sh);
}
