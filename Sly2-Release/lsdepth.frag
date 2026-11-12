#version 430 core
// PC lamp shadows (render.cpp DrawLampShadows): the distance from the lamp over its reach, as the depth (0 at the lamp,
// 1 at the end of its light), the same for every face; lsapply.frag compares a pixel's own with it.
uniform vec3 posLight;
uniform float rcpRange;
in vec3 gPosW;
void main() { gl_FragDepth = clamp(length(gPosW - posLight) * rcpRange, 0.0, 1.0); }
