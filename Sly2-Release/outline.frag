#version 430 core
// Depth-based ink lines over the 3D picture (post.cpp; display.h outlines), drawn with the original's cel-border colour
// on top of the original's own borders, which stay. The depth is the scene's (reversed Z, infinite far: near / w,
// 0 where nothing was drawn), and for a plane it is affine in screen space, so the second difference across a pixel
// is 0 on any flat surface however steeply it is seen (no dark bands on floors toward the horizon):
// - silhouettes: the pixel is much nearer than the average of two opposite neighbours (an object in front of what is
//   behind it); only the near side is inked, so the line sits on the object's edge as the original's borders do
// - creases (outlines 2): the surface's slope changes across the pixel (a wall meeting the floor, a box's edges);
//   measured per pixel angle, so the lines do not depend on the resolution
// The lines fade out with distance (fade), as the fog would hide them, and under the see-through surfaces in front of
// them (glass, water, clouds: not in the depth; texCover, render.cpp FBuildTranslucentMask) as they cover them. Alpha
// passes through.

uniform sampler2D tex;        // the picture
uniform sampler2D texDepth;   // the scene's depth (unit 1, texelFetch)
uniform float radius;         // the line's width in pixels (one PS2 pixel: renderHeight / 448)
uniform vec4 inkRgba;         // 0..1; a: the lines' opacity
uniform float sNear;          // the projection's near distance (depth = sNear / w)
uniform vec2 fade;            // view distances where the lines start and finish fading
uniform float rcpFocal;       // one pixel's angle: 2 tan(fovY / 2) / renderHeight
uniform int fCreases;
uniform sampler2D texCover;   // (PC) how opaque the see-through surfaces in front of the depth are (unit 3, R8)
uniform int fCover;           // 0: no cover this frame
in vec2 vUv;
out vec4 FragColor;

ivec2 s_lim;
float D(ivec2 p) { return texelFetch(texDepth, clamp(p, ivec2(0), s_lim), 0).r; }

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    s_lim = textureSize(tex, 0) - 1;
    vec4 c = texelFetch(tex, p, 0);
    float d0 = D(p);
    if (d0 <= 0.0)
    {
        FragColor = c;   // nothing drawn here (the clear): the sky's side of a silhouette
        return;
    }
    const ivec2 adir[4] = ivec2[4](ivec2(1, 0), ivec2(0, 1), ivec2(1, 1), ivec2(1, -1));
    float r = max(radius, 1.0);
    float sil = 0.0, crease = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        // (the diagonals at the same distance as the axes)
        float rDir = i < 2 ? r : r * 0.7071;
        ivec2 o = ivec2(round(vec2(adir[i]) * max(rDir, 1.0)));
        float a = D(p + o), b = D(p - o);
        // relative: 0 on a plane, 1 against the background, about (w far - w near) / w far in front of a wall
        float e = (2.0 * d0 - a - b) / d0;
        sil = max(sil, smoothstep(0.04, 0.12, e));
        if (fCreases != 0 && a > 0.0 && b > 0.0)
        {
            // the slope change in tan units: e over the span (pixels) and one pixel's angle; a crease of a right angle
            // seen at 45 degrees is about 2, a gently curved surface far below 1
            ivec2 oc = ivec2(round(vec2(adir[i]) * max(rDir * 0.5, 1.0)));
            float ac = D(p + oc), bc = D(p - oc);
            if (ac > 0.0 && bc > 0.0)
            {
                float k = abs(2.0 * d0 - ac - bc) / d0 / (float(max(abs(oc.x), abs(oc.y))) * rcpFocal);
                crease = max(crease, smoothstep(0.8, 1.6, k) * 0.75);
            }
        }
    }
    float w = sNear / d0;
    float ink = max(sil, crease) * inkRgba.a * (1.0 - smoothstep(fade.x, fade.y, w));
    if (fCover != 0)
    {
        // (the line's own pixel and the two beside it across: a line one pixel past a pane's edge fades with it)
        float cover = texelFetch(texCover, p, 0).r;
        cover = max(cover, max(texelFetch(texCover, clamp(p + ivec2(1, 0), ivec2(0), s_lim), 0).r,
                               texelFetch(texCover, clamp(p - ivec2(1, 0), ivec2(0), s_lim), 0).r));
        ink *= 1.0 - cover;
    }
    FragColor = vec4(mix(c.rgb, inkRgba.rgb, ink), c.a);
}
