#version 430 core
// Shadow afterimages (render.cpp RenderGhostPoses; ninjamoves.h Silent Sprint): a glob's triangles at a saved pose, as
// glob.vert places them (skinned: the weighted bone matrices, world space; else the group's matrix). The glob's own
// vertex arrays (attributes 0 / 1 / 5 / 7 of the arena); the saved bones in their own buffer.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 5) in uvec4 aBone;
layout (location = 7) in vec4 aWeight;
layout (std430, binding = 7) readonly buffer GHOSTBONES { mat4 abone[]; };
uniform mat4 matWorldToClip;
uniform mat4 matModel;
uniform int iboneFirst;
uniform int cbone;        // 0: not skinned (matModel)
out vec2 vUv;
void main()
{
    vec4 p = vec4(aPos, 1.0);
    vec4 posWorld;
    if (cbone > 0)
    {
        uint c = uint(cbone) - 1u, f = uint(iboneFirst);
        posWorld = aWeight.x * (abone[f + min(aBone.x, c)] * p) + aWeight.y * (abone[f + min(aBone.y, c)] * p)
                 + aWeight.z * (abone[f + min(aBone.z, c)] * p) + aWeight.w * (abone[f + min(aBone.w, c)] * p);
        posWorld.w = 1.0;
    }
    else posWorld = matModel * p;
    gl_Position = matWorldToClip * posWorld;
    vUv = aUv;
}
