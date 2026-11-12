#version 430 core
// PC: the characters' own pixels (render.cpp FBuildCharacterMask; with smdepth.vert): marked, for the ambient
// occlusion and the level's shadows to leave them alone.
out vec4 FragColor;
void main() { FragColor = vec4(1.0); }
