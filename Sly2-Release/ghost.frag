#version 430 core
// Shadow afterimages (render.cpp RenderGhostPoses): one flat colour, translucent. A textured pass's cut-out texels
// (alpha under 0x20 of the GS's 0x80) are left out, so fur tufts and lashes keep their outline.
uniform sampler2D tex;
uniform int fTextured;
uniform vec4 rgba;        // 0..1, straight alpha
in vec2 vUv;
out vec4 FragColor;
void main()
{
    if (fTextured != 0 && texture(tex, vUv).a < 0.125) discard;
    FragColor = rgba;
}
