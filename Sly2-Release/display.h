#pragma once

// PC display options, as in Sly1-Release (gl.cpp ApplyAspectRatioSettings / ApplyWindowModeSettings /
// FrameBufferSizeCallBack, cm.cpp RecalcCm), stored in <user-dir>/Saves/system_settings.dat through
// platform/system_settings.h (the launcher's Settings page edits the same file): aspect mode, internal resolution,
// MSAA, frame-rate cap, draw distance, window mode, VSync.
//
// The layout: the window (output) holds a centred picture of the chosen aspect (pillar- / letterboxed); the scene
// renders into an off-screen target of the picture's aspect (at the internal resolution when set); the 2D canvas
// (the PS2 frame, 640 x 448 shown at 4:3) is centred in the picture, pillarboxed on wide pictures.

#include <glm/glm.hpp>

#include <filesystem>
#include <string>

struct GLFWwindow;

// The aspect modes (values = system_settings.h AspectValue = Sly1-Release gl.h AspectMode).
enum DISPASPECT
{
    DISPASPECT_Fit = 0,         // the window's aspect; the original's vertical field of view (Hor+)
    DISPASPECT_Fixed_4_3 = 1,
    DISPASPECT_Fixed_16_9 = 2,
    DISPASPECT_Fixed_16_10 = 3,
    DISPASPECT_PS2_4_3 = 4,     // the original's anamorphic picture: the camera's rAspect squeezed into 4:3
    DISPASPECT_PS2_16_9 = 5,    // the original's horizontal field of view kept, cropped to 16:9 (Vert-)
};
enum DISPWINDOW { DISPWINDOW_Windowed = 0, DISPWINDOW_Borderless = 1, DISPWINDOW_Fullscreen = 2 };

constexpr int DISP_FRAMERATE_UNCAPPED = 1000;   // (the launcher's "Uncapped": a value of the shared field)

struct DISPLAY
{
    // Options (system_settings.dat)
    DISPASPECT aspect = DISPASPECT_Fit;
    int internalHeight = 0;     // 0: the picture's own size
    int msaaSamples = 0;        // 0 / 1: off
    int frameRate = 60;         // the frame-rate cap (>= 30; DISP_FRAMERATE_UNCAPPED and above: none)
    float drawDistance = 1.0f;  // multiplies the distance limits (MRD), 0.75 .. 3; above 1x also drawdist.h (visual only)
    DISPWINDOW window = DISPWINDOW_Windowed;
    bool fVsync = false;
    // Native QoL, always on (not in system_settings.dat; the launcher dropped its toggle 2026-11-05; --wide-backgrounds 0 or
    // SLY2N_WIDEBG=0 turns it off): the 2D's full-screen backgrounds
    // (vector polygons on the 4:3 canvas's edges: the binocucom's surround, the pause menu's backdrop, the cut
    // scenes' bars, the fades) reach the window's edges; everything else keeps the 4:3 canvas.
    bool fWideBackgrounds = true;
    // Native QoL (not in system_settings.dat; --hq-logo 0 / SLY2N_HQLOGO=0 turn it off): the front end's Sly 2 logo is
    // the launcher's high-quality one (qolview.h GlTexLogoOverride)
    bool fHqLogo = true;
    // Native QoL (system_settings.dat v16 longShadows, default on; --long-shadows 0 / SLY2N_LONGSHADOWS=0 turn it off):
    // the type-2 cast shadows (Sly's, the guards') reach the floor instead of ending at the original's fixed far cap
    // (render.cpp FBuildShadow2, render.md "Long shadows", R39). Off: the original's prisms exactly.
    bool fLongShadows = true;
    // PC image quality (system_settings.dat; the launcher's Graphics page, read at start). Off / 1 / Bilinear / 0 draw
    // exactly as before. (OpenGL on Windows and Linux only: the Switch and macOS keep the original paths.)
    // - fAlphaCoverage (v20 alphaToCoverage, default on): with MSAA, alpha-tested edges cover the samples in
    //   proportion (glob.frag ALPHA_COVERAGE; built into the glob programs at start, gl.cpp)
    // - postAa (v20, PostAaValue): FXAA over the 3D picture before the HUD (post.h)
    // - upscaler (v11, UpscalerValue) and sharpness (v11 upscalerSharpness, 0..100): Sharp: contrast-adaptive
    //   sharpening of the 3D picture (post.h) and quality scaling to the window (GL::Present)
    // - anisotropy (v11, 1 / 2 / 4 / 8 / 16): above 1 the original textures get mipmaps and trilinear filtering with
    //   this anisotropy (render.cpp TexFromSel; texture mods always have them)
    bool fAlphaCoverage = true;
    int postAa = 0;
    int upscaler = 0;
    int sharpness = 80;
    int anisotropy = 1;
    // PC picture (v21; post.h, the launcher's Graphics page). 0 / Original draw exactly as before.
    // - bloom (0..100): the glow around the picture's brightest parts (lights, windows, neon)
    // - colorGrade (ColorGradeValue): Original, Vivid (a built-in livelier grade) or Lut (Mods/ColorGrading/lut.png)
    // - vignette (0..100): the corners darkened
    // - outlines (OutlinesValue): ink lines from the depth buffer added to the original's cel borders, at the
    //   silhouettes (1) or the silhouettes and the surfaces' creases (2)
    int bloom = 0;
    int colorGrade = 0;
    int vignette = 0;
    int outlines = 0;
    // PC lighting (v22; post.h, render.cpp ShadowCombine). 0 draws exactly as before.
    // - ambientOcclusion (0..100): corners, creases and where things meet the floor darkened, in steps (cel-style)
    // - softShadows (SoftShadowsValue): the cast shadows' edges blurred over about 2 (1) or 4 (2) PS2 pixels
    int ambientOcclusion = 0;
    int softShadows = 0;
    // PC character shadow maps (settings v23 shadowMaps; --shadow-maps / SLY2N_SHADOWMAPS over it). 1: the characters'
    // cast shadows (render kind 7's type-2 casters) drawn from a depth map of their own meshes seen from the shadow's
    // light (render.cpp, shadowmap.frag) in place of the original's volumes of their shadow hulls, filtered (soft
    // edges); the darkness each surface takes is the original's (its frame-alpha level, the kind-7 ramp). 0: the volumes.
    int shadowMaps = 0;
    // PC world shadows (settings v23 worldShadows, 0..100; --world-shadows / SLY2N_WORLDSHADOWS over it). The level's
    // static meshes cast shadows from its main directional light (render.cpp DrawWorldShadows, wsapply.frag; the light's
    // direction SLY2N_WORLDSHADOWDIR "x y z" for testing), one cel tone over the picture, darker with the value. 0: off.
    // aWorldShadowsEpisode: per episode (0 the prologue .. 8) its own strength in place of worldShadows (-1: worldShadows).
    int worldShadows = 0;
    int aWorldShadowsEpisode[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    // PC character smoothing (settings v24 smoothChars / smoothStrength, the launcher's "Character smoothing"; over them
    // --smooth-chars / SLY2N_SMOOTH, --smooth-strength / SLY2N_SMOOTHSTRENGTH). The skinned meshes (the characters) drawn as PN triangles: each triangle split into
    // smoothChars x smoothChars and bent along the averaged normals of its corners (glob.geom SMOOTH), and their cel
    // borders bent the same way (celborder.vert SMOOTH), so the silhouettes round off. 0 / 1: off (exactly as before);
    // 2..4 the split. smoothStrength (0..100): how far toward the curved surface. OpenGL on Windows and Linux only,
    // with the near-plane clip stage (gl.cpp: not with SLY2N_NEARCLIP=0).
    int smoothChars = 0;
    int smoothStrength = 100;

    // The swap (ApplyDisplaySettings): VSync gates the frames at the cap (the pacer then runs 1 % fast, so only the
    // swap gates: platform/frame_pacer.h), and a generation that changes with the swap's configuration
    bool fSwapPacesFrames = false;
    unsigned swapGeneration = 0;

    // Layout (LayoutDisplay)
    int outputWidth = 1, outputHeight = 1;          // the window's framebuffer
    int presentX = 0, presentY = 0, presentWidth = 1, presentHeight = 1;   // the picture in it
    int renderWidth = 1, renderHeight = 1;          // the scene target
    float rAspectPicture = 4.0f / 3.0f;
    // The 2D canvas in the picture: NDC = canvas NDC x scale + offset.
    glm::vec2 uiScale{1.0f}, uiOffset{0.0f};
};
extern DISPLAY g_display;
// The refresh rate of the monitor holding most of the window (0: unknown)
int DisplayRefreshRate(GLFWwindow* window);

// Reads the options (a missing file is created with the defaults; a newer one is read-only). Returns false and
// keeps the defaults when the file cannot be used.
bool FLoadDisplaySettings(const std::filesystem::path& userDir, std::string* pmessage = nullptr);
// Writes the options back into the file (the other fields, key bindings and audio, kept).
bool FSaveDisplaySettings(const std::filesystem::path& userDir);
// (the debug menu's Graphics page) the picture's options as they are now into system_settings.dat (what the launcher
// shows): the post-processing, the lighting, the shadows; the character smoothing as smoothChars / smoothStrength
// (taken at the next start)
bool FSavePictureSettings(const std::filesystem::path& userDir, int smoothChars, int smoothStrength);

// The window's framebuffer changed (or an option did): the picture, the scene target (g_gl.CreateFramebuffers
// when its size or MSAA changes) and the 2D canvas.
void LayoutDisplay(int outputWidth, int outputHeight);
// Window mode, swap interval and layout for the current options (call after the window exists).
void ApplyDisplaySettings(GLFWwindow* window);
// Installs the framebuffer-size callback (LayoutDisplay).
void InstallDisplayCallbacks(GLFWwindow* window);

// The projection the picture needs, from the original camera's vertical field of view and its aspect (CM +0x3C,
// 512 / 394.24): Sly1-Release RecalcCm. (For the camera's RecalcCm / CMVIEW.)
struct DISPPROJ { float radFovY; float rAspect; };
DISPPROJ ProjectionForDisplay(float radFovY, float rAspectCm = 512.0f / 394.24f);

// --aspect / --internal-res style overrides ("fit", "4:3", "16:9", "16:10", "ps2-4:3", "ps2-16:9" or 0..5).
bool FParseAspect(const char* psz, DISPASPECT& aspect);
