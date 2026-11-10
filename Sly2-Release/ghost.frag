#version 430 core
// Shadow afterimages (render.cpp RenderGhostPoses): one flat colour, translucent. A textured pass's cut-out texels
// (alpha under 0x20 of the GS's 0x80) are left out, so fur tufts and lashes keep their outline.
uniform sampler2D tex;
uniform int fTextured;
uniform vec4 rgba;        // 0..1, straight alpha
uniform int mode;         // 0 the silhouette; 1 a glow recoloured (Leaping Dragon's red aura): the texture x vertex colour's
                          // brightness in rgba's colour, its alpha x rgba's, added
in vec2 vUv;
in vec4 vColor;
out vec4 FragColor;
void main()
{
    if (mode == 1)
    {
        vec4 c = (fTextured != 0 ? texture(tex, vUv) * 2.0 : vec4(1.0)) * vColor;   // (GS modulate: 0x80 = 1)
        float l = clamp(max(c.r, max(c.g, c.b)), 0.0, 1.0);
        FragColor = vec4(rgba.rgb * l, clamp(c.a, 0.0, 1.0) * rgba.a);
        return;
    }
    if (fTextured != 0 && texture(tex, vUv).a < 0.125) discard;
    FragColor = rgba;
}
