#pragma once

// PC image quality passes (display.h; the launcher's Graphics page). None of them is the original's: each is off by
// default or does nothing until its option is chosen, and with them off the frame is drawn exactly as before.
//
// - PostProcessScene: after the 3D scene and before the HUD (game.cpp), over the 3D picture and written back into the
//   scene target, in this order: the stepped ambient occlusion (ambientOcclusion; ao.frag into its own target,
//   aoapply.frag), the depth-based outlines (outlines; outline.frag), FXAA (postAa), contrast-adaptive
//   sharpening (upscaler Sharp, at sharpness), and last the bloom, the colour grade and the vignette in one pass
//   (bloom, colorGrade, vignette; bloomdown.frag / bloomup.frag build the glow, grade.frag adds it).
// - FPresentScaled: GL::Present's Sharp scaling of the finished picture into the window (scale.frag).
//
// OpenGL on Windows and Linux only: on the Switch (deko3d's offline programs) and macOS (the Metal translation, which
// knows the game's own programs by their uniforms) both do nothing. A program that fails to build turns its pass off
// for the session (printed once), and the frame is drawn as without it.

#include <glad/glad.h>

#include <cstdint>

struct CAMERA;

// The 3D picture's passes, into g_gl.FboScene(); the GL state the caller had (program, vertex array, framebuffers,
// texture and sampler on unit 0, viewport, scissor, blend, depth, stencil, culling, masks) is restored.
// cam: the frame's camera (the outlines' depth: its near distance and field of view); rgbaCel: the original's cel-border
// colour (RENDEROPTS, GS units), the outlines' ink.
void PostProcessScene(const CAMERA& cam, uint32_t rgbaCel);

// The Sharp scaling: texScene (renderWidth x renderHeight, bilinear) drawn into the draw framebuffer's window
// rectangle (x, y, dx, dy). False when the Sharp scaling is not chosen or not available, or the sizes are equal: the
// caller blits as before.
bool FPresentScaled(GLuint texScene, int dxSrc, int dySrc, int x, int y, int dx, int dy);

// Frees the passes' targets and programs (GL::Terminate).
void ShutdownPostProcess();
