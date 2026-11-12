#version 430 core
// PC debug (render.cpp DrawShadowCasterOverlay; with smdepth.vert): a group in one colour, the level shadows' casters
// shown over the picture, or its number into the picker's target.
uniform vec4 rgba;
out vec4 FragColor;
void main() { FragColor = rgba; }
