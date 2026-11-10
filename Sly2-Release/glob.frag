#version 430 core
// Sly 2 glob fragments: the GS's texture function (MODULATE, TCC 1) and alpha test, for one pass of a shader.
// The pass's ALPHA blend and depth test are GL state (render.cpp).
// ALPHA_COVERAGE (PC image quality, display.h fAlphaCoverage; built in at start, gl.cpp): on a multisampled target the
// threshold alpha tests (LESS / LEQUAL / GEQUAL / GREATER) cover a pixel's samples in proportion to where the alpha
// crosses the reference instead of all or none (gl_SampleMask), so MSAA smooths the cut-out edges of foliage, fences and
// grates too. The pass sub-draw takes the low samples and the AFAIL sub-draw exactly the others, so the two still
// partition the pixel. On a single-sample target, and for the other tests, the original discard as before.
// NOKILL: the variant without the alpha test and DATE (no discard), for the draws that use neither, so the GPU tests
// depth and stencil before the fragments (render.cpp picks the program per run). The uniforms stay declared in both.

uniform sampler2D tex;
uniform int  fTextured;
uniform int  ate;            // TEST: alpha test enable
uniform int  atst;           //       0 never 1 always 2 less 3 lequal 4 equal 5 gequal 6 greater 7 notequal
uniform float aref;          //       reference (GS units)
uniform int  afailMode;      // 0: no test, 1: keep fragments that pass, 2: keep fragments that fail
uniform vec4 fogColor;       // 0..1
uniform int  fFogBlack;      // the pass's FOGCOL is 0 (0x1EB960: kinds 1 / 7's additive passes), not fogColor
uniform int  fDate;          // TEST DATE, DATM 0 (the shadow hulls): only where the frame's alpha before the marking
uniform sampler2D texDate;   //   (texDate, 0x16DA48's copy) is below 0x80

#ifdef BINDLESS
flat in uvec2 vTexh;
#endif
noperspective in vec4 vColor;
in vec2 vUv;
noperspective in float vFog;

out vec4 FragColor;

bool AlphaPasses(float a)
{
    switch (atst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

void main()
{
    vec4 c = vColor;
#if defined(ALPHA_COVERAGE) && !defined(NOKILL)
    float aRaw = vColor.a;   // the alpha before the GS's floor (its gradient sets the coverage's ramp)
#endif
    if (fTextured != 0)
    {
#ifdef BINDLESS
        // (the same texture and sampler as a bound one: handle 0 is the special draws', which bind tex)
        vec4 t = (vTexh != uvec2(0u) ? texture(sampler2D(vTexh), vUv) : texture(tex, vUv)) * 255.0;
#else
        vec4 t = texture(tex, vUv) * 255.0;
#endif
        c = floor(t * vColor / 128.0);
#if defined(ALPHA_COVERAGE) && !defined(NOKILL)
        aRaw = t.a * vColor.a / 128.0;
#endif
    }
#if defined(ALPHA_COVERAGE) && !defined(NOKILL)
    // (in uniform control flow, before any discard)
    float dAlpha = fwidth(aRaw);
#endif
#ifndef NOKILL
    if (fDate != 0 && texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0) discard;
#endif
    c = clamp(c, 0.0, 255.0);
#ifndef NOKILL
    if (afailMode != 0 && ate != 0)
    {
#ifdef ALPHA_COVERAGE
        if (gl_NumSamples > 1 && (atst == 2 || atst == 3 || atst == 5 || atst == 6))
        {
            // floor(a) GEQUAL r <=> a >= r; GREATER r <=> a >= r + 1; LESS r <=> a < r; LEQUAL r <=> a < r + 1
            float thr = (atst == 3 || atst == 6) ? aref + 1.0 : aref;
            float fAbove = clamp((min(aRaw, 255.0) - thr) / max(dAlpha, 1.0 / 64.0) + 0.5, 0.0, 1.0);
            float fPass = atst >= 5 ? fAbove : 1.0 - fAbove;
            int csample = min(gl_NumSamples, 31);
            int cpass = int(floor(fPass * float(csample) + 0.5));
            int maskAll = (1 << csample) - 1;
            int maskPass = (1 << cpass) - 1;
            int mask = afailMode == 1 ? maskPass : (maskAll & ~maskPass);
            if (mask == 0) discard;
            gl_SampleMask[0] = mask;
        }
        else
#endif
        {
            bool fPass = AlphaPasses(c.a);
            if ((afailMode == 1) != fPass) discard;
        }
    }
#endif
    vec3 rgb = c.rgb / 255.0;
    rgb = mix(rgb, fFogBlack != 0 ? vec3(0.0) : fogColor.rgb, vFog);
    // The GS blends with As / 128 (0x80 = 1.0).
    FragColor = vec4(rgb, min(c.a / 128.0, 1.0));
}
