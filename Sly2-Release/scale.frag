#version 430 core
// The Sharp scaling of the finished picture into the window (post.cpp, GL::Present; display.h upscaler Sharp), in
// place of the one-sample bilinear blit:
// - up (a lower internal resolution): Catmull-Rom, sharper than bilinear, as nine bilinear taps;
// - down (supersampling: an internal resolution above the window's): a 4 x 4 Catmull-Rom kernel laid out in the
//   window's pixels (taps at 0.5 and 1.5 pixels each side, each a bilinear tap of the source), so every source pixel
//   under the window pixel counts, without the one-sample blit's shimmer.

uniform sampler2D tex;    // the picture (bilinear, clamped)
uniform vec2 srcSize;     // its size in pixels
uniform vec2 dstSize;     // the window rectangle's size in pixels
in vec2 vUv;              // 0..1 across the window rectangle
out vec4 FragColor;

vec3 CatmullRomUp(vec2 pos)
{
    vec2 t1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (t1 - 1.0) / srcSize;
    vec2 t3 = (t1 + 2.0) / srcSize;
    vec2 t12 = (t1 + w2 / w12) / srcSize;
    vec3 r = vec3(0.0);
    r += textureLod(tex, vec2(t0.x, t0.y), 0.0).rgb * w0.x * w0.y;
    r += textureLod(tex, vec2(t12.x, t0.y), 0.0).rgb * w12.x * w0.y;
    r += textureLod(tex, vec2(t3.x, t0.y), 0.0).rgb * w3.x * w0.y;
    r += textureLod(tex, vec2(t0.x, t12.y), 0.0).rgb * w0.x * w12.y;
    r += textureLod(tex, vec2(t12.x, t12.y), 0.0).rgb * w12.x * w12.y;
    r += textureLod(tex, vec2(t3.x, t12.y), 0.0).rgb * w3.x * w12.y;
    r += textureLod(tex, vec2(t0.x, t3.y), 0.0).rgb * w0.x * w3.y;
    r += textureLod(tex, vec2(t12.x, t3.y), 0.0).rgb * w12.x * w3.y;
    r += textureLod(tex, vec2(t3.x, t3.y), 0.0).rgb * w3.x * w3.y;
    return r;
}

vec3 CatmullRomDown(vec2 uv, vec2 ratio)
{
    // Catmull-Rom at 0.5 and 1.5 pixels: 0.5625 and -0.0625 (each side; the four sum to 1)
    const float aw[4] = float[](-0.0625, 0.5625, 0.5625, -0.0625);
    const float ao[4] = float[](-1.5, -0.5, 0.5, 1.5);
    vec2 stepUv = ratio / srcSize;   // one window pixel in the source's texture coordinates
    vec3 r = vec3(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r += textureLod(tex, uv + vec2(ao[i], ao[j]) * stepUv, 0.0).rgb * (aw[i] * aw[j]);
    return r;
}

void main()
{
    vec2 ratio = srcSize / dstSize;
    vec3 rgb;
    if (max(ratio.x, ratio.y) > 1.0) rgb = CatmullRomDown(vUv, max(ratio, vec2(1.0)));
    else rgb = CatmullRomUp(vUv * srcSize);
    FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
