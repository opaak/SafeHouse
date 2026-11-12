#version 430 core
// PC lamp shadows (render.cpp DrawLampShadows): each triangle into the six faces of the lamp's cube map (layers
// layerFirst .. + 5 of the cube map array), each face's 90-degree view from the lamp.
layout (triangles) in;
layout (triangle_strip, max_vertices = 18) out;
uniform mat4 amatFace[6];   // world to each face's clip
uniform int layerFirst;
in vec3 vPosW[];
out vec3 gPosW;
void main()
{
    for (int f = 0; f < 6; ++f)
    {
        vec4 c0 = amatFace[f] * vec4(vPosW[0], 1.0), c1 = amatFace[f] * vec4(vPosW[1], 1.0), c2 = amatFace[f] * vec4(vPosW[2], 1.0);
        // (wholly outside one side of the face's view: not sent)
        if ((c0.x > c0.w && c1.x > c1.w && c2.x > c2.w) || (c0.x < -c0.w && c1.x < -c1.w && c2.x < -c2.w) ||
            (c0.y > c0.w && c1.y > c1.w && c2.y > c2.w) || (c0.y < -c0.w && c1.y < -c1.w && c2.y < -c2.w) ||
            (c0.w <= 0.0 && c1.w <= 0.0 && c2.w <= 0.0))
            continue;
        gl_Layer = layerFirst + f;
        gl_Position = c0; gPosW = vPosW[0]; EmitVertex();
        gl_Position = c1; gPosW = vPosW[1]; EmitVertex();
        gl_Position = c2; gPosW = vPosW[2]; EmitVertex();
        EndPrimitive();
    }
}
