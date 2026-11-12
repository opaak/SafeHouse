#version 430 core
// PC: the see-through surfaces' cover (render.cpp FBuildTranslucentMask; with tmask.vert): how opaque the surface in
// front of the frame's depth is here (the GS's MODULATE alpha, 0x80 = opaque), kept as the greatest of those drawn
// (blended with MAX). The depth-based outlines (outline.frag) fade under it.
uniform sampler2D tex;
uniform int fTextured;
uniform float rAlpha;     // the object's fade, or the blend's fixed alpha
in vec2 vUv;
noperspective in float vA;
out vec4 FragColor;
void main()
{
    float a = vA;
    if (fTextured != 0) a = floor(texture(tex, vUv).a * 255.0 * vA / 128.0);
    FragColor = vec4(clamp(a / 128.0, 0.0, 1.0) * rAlpha);
}
