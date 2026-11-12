#version 430 core
// PC lamp shadows (render.cpp DrawLampShadows; display.h lampShadows): every pixel of the frame, its world position
// from the scene's depth, looked up in the cube maps of the few lamps chosen this frame (the level's point lights:
// their light ends where distanceOffset + distance x distanceScale reaches 0). Behind a caster from a lamp it is in
// that lamp's shadow, as darker as the lamp lights it there (its fall-off) and the lamp's own weight (it fades in when
// it is chosen); one cel step, as wsapply.frag's. Surfaces turned away from a lamp are left as the art painted them,
// and the characters' own pixels (texMask) alone. Only the colour is drawn (blend 0 x Cs + (1 - As) x Cd).

uniform sampler2D texDepth;               // unit 3: the scene's depth (reversed Z, [0, 1]; 0 nothing drawn)
uniform samplerCubeArrayShadow texLs;     // unit 4: the lamps' cube maps (6 layers each, LEQUAL compare)
uniform sampler2D texMask;                // unit 5: the characters' own pixels
uniform int fMask;
uniform mat4 matClipToWorld;
uniform vec2 viewSize;
uniform int lampCount;                      // the lamps this frame (0..4)
uniform vec4 alamp[4];                    // xyz the lamp, w its reach
uniform vec4 aatt[4];                     // x distanceOffset, y distanceScale, z the lamp's weight (0..1)
uniform float rcpFace;                    // one texel of a face (2 / resolution, in the face's tan units)
uniform float strength;                   // 0..1: how dark (x 0.5)
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
    if (d0 <= 0.0) discard;
    if (fMask != 0 && texelFetch(texMask, p, 0).r > 0.5) discard;
    vec3 P = WorldAt(p, d0);
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
    vec3 toEye = WorldAt(p, 1.0) - P;
    if (dot(N, toEye) < 0.0) N = -N;

    float shAll = 0.0;
    for (int i = 0; i < lampCount; ++i)
    {
        vec3 toL = alamp[i].xyz - P;
        float dist = length(toL);
        float range = alamp[i].w;
        if (dist >= range || dist < 1.0) continue;
        vec3 L = toL / dist;
        float ndl = dot(N, L);
        float facing = smoothstep(0.02, 0.25, ndl);   // (turned away from the lamp: already the art's shade)
        if (facing <= 0.0) continue;
        float att = clamp(aatt[i].x + dist * aatt[i].y, 0.0, 1.0);
        if (att <= 0.0) continue;
        // a little off the surface (more where the light grazes it, and farther from the lamp: a texel is wider there)
        float texel = dist * rcpFace;
        vec3 Po = P + N * texel * (1.5 + 2.0 * (1.0 - ndl));
        vec3 dirL = Po - alamp[i].xyz;
        float ref = length(dirL) / range - 0.0015;
        // 4 taps round the direction (a texel apart across it), each a hardware 2 x 2
        vec3 dn = normalize(dirL);
        vec3 t = normalize(cross(dn, abs(dn.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(dn, t);
        float lit = 0.0;
        lit += texture(texLs, vec4(dn + (t + b) * rcpFace * 0.5, float(i)), ref);
        lit += texture(texLs, vec4(dn + (t - b) * rcpFace * 0.5, float(i)), ref);
        lit += texture(texLs, vec4(dn + (-t + b) * rcpFace * 0.5, float(i)), ref);
        lit += texture(texLs, vec4(dn + (-t - b) * rcpFace * 0.5, float(i)), ref);
        float sh = smoothstep(0.3, 0.7, 1.0 - lit * 0.25);
        sh *= facing * smoothstep(0.0, 0.35, att) * aatt[i].z;
        shAll = max(shAll, sh);
    }
    if (shAll <= 0.0) discard;
    FragColor = vec4(0.0, 0.0, 0.0, strength * 0.5 * shAll);
}
