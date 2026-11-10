#version 430 core
// Post-process anti-aliasing over the 3D picture (post.cpp; display.h postAa FXAA): a luma edge search in the manner
// of FXAA's quality preset. Each pixel on a high-contrast edge finds the edge's direction and its two ends, then
// samples the picture shifted across the edge by how far the pixel is from the nearer end; flat areas are copied as
// they are. The frame's alpha passes through unchanged (the HUD's blending after this may read it).

uniform sampler2D tex;    // the picture (bilinear, clamped)
uniform vec2 rcpSize;     // 1 / its size in pixels
in vec2 vUv;
out vec4 FragColor;

const float kEdgeMin = 0.0312;        // no AA below this luma contrast ...
const float kEdgeRel = 0.125;         // ... or below this fraction of the local maximum
const float kSubpixel = 0.5;          // how much single-pixel features are softened (0 off .. 1)
const int kSteps = 12;
const float kStep[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float LumaAt(vec2 uv) { return Luma(textureLod(tex, uv, 0.0).rgb); }

void main()
{
    vec2 uv = vUv;
    vec4 cM = textureLod(tex, uv, 0.0);
    float lM = Luma(cM.rgb);
    float lN = Luma(textureLodOffset(tex, uv, 0.0, ivec2(0, 1)).rgb);
    float lS = Luma(textureLodOffset(tex, uv, 0.0, ivec2(0, -1)).rgb);
    float lE = Luma(textureLodOffset(tex, uv, 0.0, ivec2(1, 0)).rgb);
    float lW = Luma(textureLodOffset(tex, uv, 0.0, ivec2(-1, 0)).rgb);
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float range = lMax - lMin;
    if (range < max(kEdgeMin, lMax * kEdgeRel))
    {
        FragColor = cM;
        return;
    }
    float lNW = Luma(textureLodOffset(tex, uv, 0.0, ivec2(-1, 1)).rgb);
    float lNE = Luma(textureLodOffset(tex, uv, 0.0, ivec2(1, 1)).rgb);
    float lSW = Luma(textureLodOffset(tex, uv, 0.0, ivec2(-1, -1)).rgb);
    float lSE = Luma(textureLodOffset(tex, uv, 0.0, ivec2(1, -1)).rgb);

    // An edge running horizontally changes along y (and the reverse).
    float edgeHorz = abs(lN + lS - 2.0 * lM) * 2.0 + abs(lNW + lSW - 2.0 * lW) + abs(lNE + lSE - 2.0 * lE);
    float edgeVert = abs(lW + lE - 2.0 * lM) * 2.0 + abs(lNW + lNE - 2.0 * lN) + abs(lSW + lSE - 2.0 * lS);
    bool fHorz = edgeHorz >= edgeVert;

    // The side of the pixel the edge lies on: the neighbour across it with the larger step.
    float l1 = fHorz ? lS : lW;
    float l2 = fHorz ? lN : lE;
    float grad1 = l1 - lM, grad2 = l2 - lM;
    bool fSide1 = abs(grad1) >= abs(grad2);
    float gradScaled = 0.25 * max(abs(grad1), abs(grad2));
    float stepLength = fHorz ? rcpSize.y : rcpSize.x;
    float lLocal;
    if (fSide1) { stepLength = -stepLength; lLocal = 0.5 * (l1 + lM); }
    else lLocal = 0.5 * (l2 + lM);

    // Walk along the edge, half a pixel toward it, both ways until the luma leaves the edge's average.
    vec2 uvEdge = uv;
    if (fHorz) uvEdge.y += stepLength * 0.5; else uvEdge.x += stepLength * 0.5;
    vec2 dir = fHorz ? vec2(rcpSize.x, 0.0) : vec2(0.0, rcpSize.y);
    vec2 uv1 = uvEdge - dir, uv2 = uvEdge + dir;
    float lEnd1 = LumaAt(uv1) - lLocal, lEnd2 = LumaAt(uv2) - lLocal;
    bool fDone1 = abs(lEnd1) >= gradScaled, fDone2 = abs(lEnd2) >= gradScaled;
    for (int i = 1; i < kSteps && !(fDone1 && fDone2); ++i)
    {
        if (!fDone1) { uv1 -= dir * kStep[i]; lEnd1 = LumaAt(uv1) - lLocal; fDone1 = abs(lEnd1) >= gradScaled; }
        if (!fDone2) { uv2 += dir * kStep[i]; lEnd2 = LumaAt(uv2) - lLocal; fDone2 = abs(lEnd2) >= gradScaled; }
    }
    float dist1 = fHorz ? uv.x - uv1.x : uv.y - uv1.y;
    float dist2 = fHorz ? uv2.x - uv.x : uv2.y - uv.y;
    bool fNearer1 = dist1 < dist2;
    float distNear = min(dist1, dist2);
    float edgeLength = dist1 + dist2;
    float offset = -distNear / edgeLength + 0.5;
    // Only toward the edge when the nearer end's luma departs the way the centre does.
    bool fCentreSmaller = lM < lLocal;
    bool fCorrect = ((fNearer1 ? lEnd1 : lEnd2) < 0.0) != fCentreSmaller;
    float offsetEdge = fCorrect ? offset : 0.0;

    // Sub-pixel aliasing: a lone bright or dark pixel among its neighbours.
    float lAvg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    float offsetSub = sub * sub * kSubpixel;

    float offsetFinal = max(offsetEdge, offsetSub);
    vec2 uvFinal = uv;
    if (fHorz) uvFinal.y += offsetFinal * stepLength; else uvFinal.x += offsetFinal * stepLength;
    FragColor = vec4(textureLod(tex, uvFinal, 0.0).rgb, cM.a);
}
