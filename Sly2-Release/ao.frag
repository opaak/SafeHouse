#version 430 core
// Ambient occlusion (post.cpp; display.h ambientOcclusion), its first pass: how open each pixel's surface is, 1 open
// .. 0 closed in, into an R8 target that aoapply.frag blurs, steps and applies. From the scene's depth alone (reversed
// Z, infinite far: depth = sNear / w, 0 where nothing was drawn): the pixel's view-space position, its normal from the
// nearer neighbour of each side (no smearing across silhouettes), and 12 points in the hemisphere over it, radius world
// units out, turned per pixel by interleaved gradient noise (the blur evens it out). A point counts as occluded when
// the scene's surface at its pixel is in front of it, weighted down when that surface is far beyond the radius (a
// wall far behind an object does not darken it). Fades out with distance (fade), as the fog would hide it.

uniform sampler2D texDepth;   // unit 1 (texelFetch)
uniform float sNear;          // depth = sNear / w
uniform vec2 tanHalf;         // tan of half the field of view across and up
uniform float radius;         // the hemisphere's radius, world units
uniform vec2 fade;            // view distances where it starts and finishes fading
out vec4 FragColor;

ivec2 s_size;
float D(ivec2 p) { return texelFetch(texDepth, clamp(p, ivec2(0), s_size - 1), 0).r; }
vec3 ViewPos(vec2 pix, float d)
{
    float w = sNear / d;
    vec2 ndc = pix / vec2(s_size) * 2.0 - 1.0;
    return vec3(ndc * tanHalf * w, -w);
}

void main()
{
    s_size = textureSize(texDepth, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d0 = D(p);
    if (d0 <= 0.0)
    {
        FragColor = vec4(1.0);
        return;
    }
    vec2 pc = vec2(p) + 0.5;
    vec3 P = ViewPos(pc, d0);
    float w0 = -P.z;
    if (w0 > fade.y)
    {
        FragColor = vec4(1.0);
        return;
    }

    // the normal: across and up, each from the neighbour whose depth is nearer this pixel's
    float dl = D(p - ivec2(1, 0)), dr = D(p + ivec2(1, 0)), db = D(p - ivec2(0, 1)), dt = D(p + ivec2(0, 1));
    bool fRight = dr > 0.0 && (dl <= 0.0 || abs(dr - d0) < abs(d0 - dl));
    bool fUp = dt > 0.0 && (db <= 0.0 || abs(dt - d0) < abs(d0 - db));
    if ((!fRight && dl <= 0.0) || (!fUp && db <= 0.0))
    {
        FragColor = vec4(1.0);   // (a lone pixel against the sky)
        return;
    }
    vec3 ddx = fRight ? ViewPos(pc + vec2(1.0, 0.0), dr) - P : P - ViewPos(pc - vec2(1.0, 0.0), dl);
    vec3 ddy = fUp ? ViewPos(pc + vec2(0.0, 1.0), dt) - P : P - ViewPos(pc - vec2(0.0, 1.0), db);
    vec3 N = normalize(cross(ddx, ddy));
    if (dot(N, -P) < 0.0) N = -N;

    const vec3 akernel[12] = vec3[12](
        vec3( 0.53,  0.11, 0.31), vec3(-0.21,  0.42, 0.18), vec3(-0.35, -0.47, 0.42), vec3( 0.12, -0.29, 0.11),
        vec3( 0.71,  0.38, 0.45), vec3(-0.62,  0.05, 0.27), vec3( 0.04,  0.68, 0.52), vec3( 0.33, -0.72, 0.36),
        vec3(-0.18, -0.15, 0.85), vec3( 0.25,  0.21, 0.66), vec3(-0.81, -0.33, 0.21), vec3( 0.47, -0.08, 0.79));
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float ang = noise * 6.2831853;
    vec3 rv = vec3(cos(ang), sin(ang), 0.0);
    vec3 T = rv - N * dot(rv, N);
    T = dot(T, T) > 1e-6 ? normalize(T) : normalize(cross(N, vec3(0.0, 0.0, 1.0)));
    vec3 B = cross(N, T);
    float bias = radius * 0.03 + w0 * 0.001;
    float occ = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        vec3 k = akernel[i];
        vec3 S = P + (T * k.x + B * k.y + N * k.z) * radius;
        float wS = -S.z;
        if (wS <= 1.0) continue;
        vec2 ndc = S.xy / (wS * tanHalf);
        ivec2 q = ivec2((ndc * 0.5 + 0.5) * vec2(s_size));
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, s_size))) continue;
        float ds = D(q);
        if (ds <= 0.0) continue;   // (the sky occludes nothing)
        float ws = sNear / ds;
        float range = smoothstep(0.0, 1.0, radius / max(abs(w0 - ws), 1e-3));
        occ += (ws < wS - bias ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - occ / 12.0;
    ao = mix(ao, 1.0, smoothstep(fade.x, fade.y, w0));
    FragColor = vec4(ao, ao, ao, 1.0);
}
