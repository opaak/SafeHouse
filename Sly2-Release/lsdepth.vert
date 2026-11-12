#version 430 core
// PC lamp shadows (render.cpp DrawLampShadows): a caster's triangles in world space (skinned: the weighted bone
// matrices; else the group's matrix), for lsdepth.geom to draw into the six faces of a lamp's cube map.
layout (location = 0) in vec3 aPos;
layout (location = 5) in uvec4 aBone;
layout (location = 7) in vec4 aWeight;
layout (std430, binding = 7) readonly buffer SMBONES { mat4 abone[]; };
uniform mat4 matModel;
uniform int iboneFirst;
uniform int cbone;        // 0: not skinned (matModel)
out vec3 vPosW;
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
    vPosW = posWorld.xyz;
    gl_Position = posWorld;
}
