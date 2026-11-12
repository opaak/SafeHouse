#version 430 core
// PC character shadow maps (render.cpp RenderShadowMaps; display.h shadowMaps): a caster's triangles into the depth
// map, as ghost.vert places them (skinned: the weighted bone matrices, world space; else the group's matrix), seen
// from the shadow's light (matWorldToLight: the caster's tile of the atlas).
layout (location = 0) in vec3 aPos;
layout (location = 5) in uvec4 aBone;
layout (location = 7) in vec4 aWeight;
layout (std430, binding = 7) readonly buffer SMBONES { mat4 abone[]; };
uniform mat4 matWorldToLight;
uniform mat4 matModel;
uniform int iboneFirst;
uniform int cbone;        // 0: not skinned (matModel)
uniform vec3 posEye;      // (the characters' mask, render.cpp FBuildCharacterMask) the camera, and how far toward it
uniform float sTowardEye; // the surface is drawn (0: in place; the shadow maps' and the world shadows' casters)
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
    if (sTowardEye > 0.0)
    {
        vec3 d = posEye - posWorld.xyz;
        float l = length(d);
        if (l > 1e-3) posWorld.xyz += d * (min(sTowardEye, 0.5 * l) / l);
    }
    gl_Position = matWorldToLight * posWorld;
}
