#pragma once

// PC image quality passes (display.h; the launcher's Graphics page). None of them is the original's: each is off by
// default or does nothing until its option is chosen, and with them off the frame is drawn exactly as before.
//
// - PostProcessScene: after the 3D scene and before the HUD (game.cpp), FXAA (postAa) and contrast-adaptive
//   sharpening (upscaler Sharp, at sharpness) over the 3D picture, written back into the scene target.
// - FPresentScaled: GL::Present's Sharp scaling of the finished picture into the window (scale.frag).
//
// OpenGL on Windows and Linux only: on the Switch (deko3d's offline programs) and macOS (the Metal translation, which
// knows the game's own programs by their uniforms) both do nothing. A program that fails to build turns its pass off
// for the session (printed once), and the frame is drawn as without it.

#include <glad/glad.h>

// The 3D picture's passes, into g_gl.FboScene(); the GL state the caller had (program, vertex array, framebuffers,
// texture and sampler on unit 0, viewport, scissor, blend, depth, stencil, culling, masks) is restored.
void PostProcessScene();

// The Sharp scaling: texScene (renderWidth x renderHeight, bilinear) drawn into the draw framebuffer's window
// rectangle (x, y, dx, dy). False when the Sharp scaling is not chosen or not available, or the sizes are equal: the
// caller blits as before.
bool FPresentScaled(GLuint texScene, int dxSrc, int dySrc, int x, int y, int dx, int dy);

// Frees the passes' targets and programs (GL::Terminate).
void ShutdownPostProcess();
