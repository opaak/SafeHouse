#version 430 core
// PC: the see-through surfaces' cover (render.cpp FBuildTranslucentMask; with tmask.frag): a group placed as
// smdepth.vert places it (skinned: the weighted bone matrices), with its texture coordinates and the alpha glob.vert
// gives it (the vertex colour's, or the pass's constant colour's).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec4 aRgbaA;   // unnormalized bytes
layout (location = 3) in vec4 aRgbaB;
layout (location = 5) in uvec4 aBone;
layout (location = 7) in vec4 aWeight;
layout (std430, binding = 7) readonly buffer SMBONES { mat4 abone[]; };
uniform mat4 matWorldToLight;
uniform mat4 matModel;
uniform int iboneFirst;
uniform int cbone;        // 0: not skinned (matModel)
uniform int icolor;       // 0 colour A, 1 colour B, 2 the constant, 3 the constant faded by colour A's alpha
uniform float aConst;     // the constant colour's alpha (GS units)
out vec2 vUv;
noperspective out float vA;
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
    gl_Position = matWorldToLight * posWorld;
    vUv = aUv;
    vA = icolor == 0 ? aRgbaA.a : icolor == 1 ? aRgbaB.a : icolor == 2 ? aConst : aConst * aRgbaA.a / 128.0;
}
