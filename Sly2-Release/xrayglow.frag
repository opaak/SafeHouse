#version 430 core
// PC: a glowing outer edge round objects seen through walls (render.cpp RenderXrayOutlines; Treasure Sense's clue
// bottles, ancestors.cpp). texMask holds the objects' own pixels, drawn without the depth test (each its brightness);
// outside them, within radius pixels, the glow: brightest at the edge, fading out, from the brightest object near;
// inside them a faint fill, so the shape reads. Added over the frame (blend As x Cs + Cd); the alpha kept.
uniform sampler2D texMask;   // R8: the objects' pixels (their brightness, 0..1)
uniform vec3 rgb;
uniform float radius;        // the glow's width in pixels
out vec4 FragColor;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = textureSize(texMask, 0) - 1;
    float m0 = texelFetch(texMask, p, 0).r;
    if (m0 > 0.0)
    {
        FragColor = vec4(rgb, 0.10 * m0);
        return;
    }
    float g = 0.0;
    const int cDir = 16;
    float step = max(1.0, radius / 6.0);
    for (int k = 0; k < cDir; ++k)
    {
        float a = float(k) * (6.2831853 / float(cDir));
        vec2 d = vec2(cos(a), sin(a));
        for (float r = 1.0; r <= radius; r += step)
        {
            float m = texelFetch(texMask, clamp(p + ivec2(round(d * r)), ivec2(0), lim), 0).r;
            if (m > 0.0)
            {
                float u = 1.0 - (r - 1.0) / radius;
                g = max(g, m * u * u);
                break;
            }
        }
    }
    if (g <= 0.0) discard;
    FragColor = vec4(rgb, g);
}
