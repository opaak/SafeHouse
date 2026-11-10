#version 430 core
// The picture's last pass before the HUD (post.cpp; display.h bloom, colorGrade, vignette): the bloom added (a screen
// blend, so bright parts glow without clipping to flat white), the colour grade, then the vignette. Alpha passes
// through (the HUD's and the overlays' passes read the scene target after this).

uniform sampler2D tex;        // the picture
uniform sampler2D texBloom;   // unit 1: the bloom chain's largest level (the sum of cLevels levels)
uniform sampler3D texLut;     // unit 2: grade 2's LUT (red along x, green along y, blue along z)
uniform float bloom;          // the bloom's strength (0: none), already divided by the chain's levels
uniform int grade;            // 0 original, 1 vivid, 2 the LUT
uniform float lutSize;        // the LUT's edge (16, 32, 64)
uniform float vignette;       // 0..1
uniform float aspect;         // the picture's width / height
in vec2 vUv;
out vec4 FragColor;

void main()
{
    vec4 c = texelFetch(tex, ivec2(gl_FragCoord.xy), 0);
    vec3 rgb = c.rgb;
    if (bloom > 0.0)
    {
        vec3 b = min(texture(texBloom, vUv).rgb * bloom, vec3(1.0));
        rgb = 1.0 - (1.0 - rgb) * (1.0 - b);
    }
    if (grade == 1)
    {
        // Vivid: a little more saturation and a gentle S-curve, closer to Sly 4's punchier colours; the blacks and
        // the whites stay where they are
        float l = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        rgb = clamp(mix(vec3(l), rgb, 1.18), 0.0, 1.0);
        rgb = mix(rgb, rgb * rgb * (3.0 - 2.0 * rgb), 0.3);
    }
    else if (grade == 2)
    {
        vec3 uvw = clamp(rgb, 0.0, 1.0) * ((lutSize - 1.0) / lutSize) + 0.5 / lutSize;
        rgb = texture(texLut, uvw).rgb;
    }
    if (vignette > 0.0)
    {
        // measured in a 16:9 frame centred on the picture, so ultrawide pictures darken their far sides, not the
        // middle third
        vec2 q = (vUv - 0.5) * 2.0;
        q.x *= aspect / (16.0 / 9.0);
        float r = length(q) * 0.7071;
        rgb *= 1.0 - vignette * 0.6 * smoothstep(0.45, 1.1, r);
    }
    FragColor = vec4(rgb, c.a);
}
