#version 430 core
// Sly 2 glob triangles clipped against the near plane the way the original's VU1 program does it, before the GS draws
// them (R51). The GS interpolates RGBA and F affinely in screen space (glob.vert: noperspective), but only between the
// vertices it is sent: P_G clips a triangle that crosses the near plane in 3D, so a new vertex on the plane carries the
// colour linear in the clip-space segment (the reference's GS log at Trojan Tank: alphas 104..117 on the clip vertices
// of the ramp under the camera, between its vertices' 70..127). GL's own clipper gives a noperspective output at a
// clip vertex the value that varies linearly in screen space over the unclipped triangle instead (GL 4.6 13.7), which
// for a vertex far behind the camera extrapolates: the floor under a low camera went dark and its frame alpha (the
// shadow combine's darkness) to 0. Here the near plane is w - z >= 0 (render.cpp: reversed-Z, z = near); a triangle
// wholly in front is passed through unchanged.

layout (triangles) in;
#ifdef SMOOTH
layout (triangle_strip, max_vertices = SMOOTH_MAXV) out;
#else
layout (triangle_strip, max_vertices = 4) out;
#endif

noperspective in vec4 gsColor[];
in vec2 gsUv[];
noperspective in float gsFog[];
#ifdef BINDLESS
flat in uvec2 gsTexh[];
flat out uvec2 vTexh;
#endif

noperspective out vec4 vColor;
out vec2 vUv;
noperspective out float vFog;

void EmitAt(int i, int j, float t)
{
    gl_Position = mix(gl_in[i].gl_Position, gl_in[j].gl_Position, t);
    vColor = mix(gsColor[i], gsColor[j], t);
    vUv = mix(gsUv[i], gsUv[j], t);
    vFog = mix(gsFog[i], gsFog[j], t);
#ifdef BINDLESS
    vTexh = gsTexh[0];
#endif
    EmitVertex();
}

#ifdef SMOOTH
// (TEST, character smoothing: display.h smoothChars) A skinned triangle wholly in front of the near plane, with an
// averaged normal at each corner (glob.vert), drawn as a PN triangle (Vlachos et al. 2001, curved point-normal
// triangles): the cubic patch through its corners with their normals as the tangent planes, split SMOOTH x SMOOTH
// and pulled SMOOTH_STRENGTH of the way from the flat triangle to it. An edge's curve depends only on its two corners,
// so the triangles beside it bend it alike (no cracks), and celborder.vert bends the cel borders' edges the same way.
// The colours, coordinates and fog go linearly over the original triangle.
in vec3 gsPosW[];
in vec3 gsNrmW[];
uniform mat4 matWorldToClip;

vec3 b210 = vec3(0.0), b120 = vec3(0.0), b021 = vec3(0.0), b012 = vec3(0.0), b102 = vec3(0.0), b201 = vec3(0.0), b111 = vec3(0.0);

void EmitBary(vec3 bc)
{
    const float u = bc.x, v = bc.y, w = bc.z;
    const vec3 p0 = gsPosW[0], p1 = gsPosW[1], p2 = gsPosW[2];
    const vec3 pFlat = u * p0 + v * p1 + w * p2;
    const vec3 pn = p0 * (u * u * u) + p1 * (v * v * v) + p2 * (w * w * w)
                  + b210 * (3.0 * u * u * v) + b120 * (3.0 * u * v * v) + b201 * (3.0 * u * u * w)
                  + b021 * (3.0 * v * v * w) + b102 * (3.0 * u * w * w) + b012 * (3.0 * v * w * w)
                  + b111 * (6.0 * u * v * w);
    gl_Position = matWorldToClip * vec4(mix(pFlat, pn, SMOOTH_STRENGTH), 1.0);
    // (the corners exactly as the vertex program computed them: the cel borders' depth test matches there)
    if (u == 1.0) gl_Position = gl_in[0].gl_Position;
    else if (v == 1.0) gl_Position = gl_in[1].gl_Position;
    else if (w == 1.0) gl_Position = gl_in[2].gl_Position;
    vColor = u * gsColor[0] + v * gsColor[1] + w * gsColor[2];
    vUv = u * gsUv[0] + v * gsUv[1] + w * gsUv[2];
    vFog = u * gsFog[0] + v * gsFog[1] + w * gsFog[2];
#ifdef BINDLESS
    vTexh = gsTexh[0];
#endif
    EmitVertex();
}

// The point at row r (0 at corner 0 .. SMOOTH at the opposite edge) and column c (0..r) of the split: rows step from
// corner 0, columns from corner 2 toward corner 1, which keeps the original's winding in the strips below
vec3 BaryAt(int r, int c)
{
    const float n = float(SMOOTH);
    return vec3(1.0 - float(r) / n, float(c) / n, float(r - c) / n);
}

bool FSmoothed()
{
    for (int i = 0; i < 3; ++i)
        if (dot(gsNrmW[i], gsNrmW[i]) < 0.25) return false;
    const vec3 p0 = gsPosW[0], p1 = gsPosW[1], p2 = gsPosW[2];
    const vec3 n0 = gsNrmW[0], n1 = gsNrmW[1], n2 = gsNrmW[2];
    b210 = (2.0 * p0 + p1 - dot(p1 - p0, n0) * n0) / 3.0;
    b120 = (2.0 * p1 + p0 - dot(p0 - p1, n1) * n1) / 3.0;
    b021 = (2.0 * p1 + p2 - dot(p2 - p1, n1) * n1) / 3.0;
    b012 = (2.0 * p2 + p1 - dot(p1 - p2, n2) * n2) / 3.0;
    b102 = (2.0 * p2 + p0 - dot(p0 - p2, n2) * n2) / 3.0;
    b201 = (2.0 * p0 + p2 - dot(p2 - p0, n0) * n0) / 3.0;
    const vec3 e = (b210 + b120 + b021 + b012 + b102 + b201) / 6.0;
    const vec3 c = (p0 + p1 + p2) / 3.0;
    b111 = e + (e - c) * 0.5;
    return true;
}
#endif

void main()
{
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = gl_in[i].gl_Position.w - gl_in[i].gl_Position.z;
    if (d[0] >= 0.0 && d[1] >= 0.0 && d[2] >= 0.0)
    {
#ifdef SMOOTH
        if (FSmoothed())
        {
            for (int r = 0; r < SMOOTH; ++r)
            {
                // the strip between rows r and r + 1: r + 2 points below, r + 1 above, alternating
                for (int c = 0; c <= r; ++c)
                {
                    EmitBary(BaryAt(r + 1, c));
                    EmitBary(BaryAt(r, c));
                }
                EmitBary(BaryAt(r + 1, r + 1));
                EndPrimitive();
            }
            return;
        }
#endif
        for (int i = 0; i < 3; ++i) EmitAt(i, i, 0.0);
        EndPrimitive();
        return;
    }
    // Sutherland-Hodgman against the one plane: up to four vertices, each (i, j, t), in the triangle's order
    ivec2 aij[4];
    float at[4];
    int c = 0;
    for (int i = 0; i < 3; ++i)
    {
        const int j = (i + 1) % 3;
        if (d[i] >= 0.0) { aij[c] = ivec2(i, i); at[c] = 0.0; ++c; }
        if ((d[i] >= 0.0) != (d[j] >= 0.0)) { aij[c] = ivec2(i, j); at[c] = d[i] / (d[i] - d[j]); ++c; }
    }
    if (c < 3) return;
    // (a strip of the polygon a b c [d]: a b d c)
    EmitAt(aij[0].x, aij[0].y, at[0]);
    EmitAt(aij[1].x, aij[1].y, at[1]);
    if (c == 4) EmitAt(aij[3].x, aij[3].y, at[3]);
    EmitAt(aij[2].x, aij[2].y, at[2]);
    EndPrimitive();
}
