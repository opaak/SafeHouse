#version 430 core
// Shadow afterimages (render.cpp RenderGhostPoses; ninjamoves.h Silent Sprint): a glob's triangles at a saved pose, as
// glob.vert places them (skinned: the weighted bone matrices, world space; else the group's matrix). The glob's own
// vertex arrays (attributes 0 / 1 / 5 / 7 of the arena); the saved bones in their own buffer.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec4 aRgbaA;   // the vertex colour (GS units: 0x80 = 1; the glow mode's)
layout (location = 5) in uvec4 aBone;
layout (location = 7) in vec4 aWeight;
layout (std430, binding = 7) readonly buffer GHOSTBONES { mat4 abone[]; };
uniform mat4 matWorldToClip;
uniform mat4 matModel;
uniform int iboneFirst;
uniform int cbone;        // 0: not skinned (matModel)
uniform vec3 posEye;      // (over its own surfaces, GHOSTDRAW fOver) the camera, and how far toward it the surface
uniform float sTowardEye; // is drawn: the frame drew a smoothed character bulging out of these triangles (0: in place)
out vec2 vUv;
out vec4 vColor;
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
    gl_Position = matWorldToClip * posWorld;
    vUv = aUv;
    vColor = aRgbaA / 128.0;
}
