# Features

[Back to the README](../README.md)

SafeHouse plays the games as the PlayStation 2 originals did, and adds options a PC player expects. Every addition
can be turned off where it changes how the game looks or plays.

## The launcher

- **One window for every game:** Sly 1, Sly 2 and Sly 3 (plus Thieves in Time and Bentley's Hackpack, listed for
  later). Mouse, keyboard and controller navigation, with on-screen keyboard and button prompts for the controller in
  use.
- **Install from your disc image:** pick or drag in an `.iso`. The launcher identifies the game and the region, checks
  the free space and that the image is complete, then installs and verifies it. An image of another game is refused
  with an explanation.
- **Several regions:** Sly 1 from the USA, Europe, Japan and Korea; Sly 2 from the USA, Europe (v1.00 and v2.01) and
  Japan. With more than one region installed you choose which one **Play** starts; saves are shared.
- **Languages:** the European discs' languages are offered in the game's own options.
- **Add to Steam:** adds a shortcut that starts the game directly, with library artwork, to every Steam account on the
  computer (exit Steam first, or restart it afterwards). **Add desktop shortcut** does the same on the desktop.
- **Settings for each game:** display, graphics, audio, controls and more, edited while the game is closed.
- **Automatic updates:** the launcher checks for new versions and updates itself.
- **Your files stay put:** installed games, saves and settings live in your profile folder and survive updates and
  re-installs ([where](FAQ.md#where-are-my-files)).

## Display and performance

- **Widescreen and super ultrawide:** any aspect ratio up to 32:9. The 3D picture widens; menus and other 2D screens
  stay at 4:3 in the middle, as the originals drew them.
- **Frame rate:** 30, 60, 90, 120 or 240 FPS, or the monitor's maximum refresh rate. The game runs at the same speed
  at every setting. VSync, MSAA and the internal resolution are options too.
- **Draw distance (Sly 2):** an option to draw distant objects further than the original.
- **Long shadows (Sly 2):** characters' shadows from low lights (the title screen's window, some interiors) reach the
  floor in full instead of ending in a straight cut, which shows most at wide aspect ratios.
  **Settings > Graphics > Long shadows**; off gives the original's shadows exactly.
- **Image quality (Sly 2, Windows and Linux):** in **Settings > Graphics**, each off or at the original by default:
  - **Smooth cut-out edges:** with MSAA on, the edges of foliage, fences and grates are smoothed too (alpha to
    coverage). With MSAA off the game draws exactly as before.
  - **Post-process anti-aliasing:** FXAA over the 3D picture, before the HUD, so menus and text stay crisp.
  - **Texture filtering:** mipmaps and trilinear filtering with 2x to 16x anisotropic filtering for the original
    textures, so distant ground and walls no longer shimmer.
  - **Image scaling: Sharp:** contrast-adaptive sharpening of the 3D picture (with a strength setting) and a better
    filter to the window: Catmull-Rom when scaling up, and a wide filter when scaling down, which makes an internal
    resolution above the screen's (supersampling) worth using; the launcher then offers those heights too.
- **Picture (Sly 2, Windows and Linux):** also in **Settings > Graphics**, each off by default:
  - **Outlines:** ink lines from the scene's depth on top of the original's cel borders, in the borders' own colour,
    closer to Sly 4's look: at the silhouettes, or also at the creases inside a shape (a wall meeting the floor). They
    fade out with distance.
  - **Bloom:** a soft glow around the brightest parts of the picture (lights, windows, neon), before the HUD.
  - **Color grading:** Vivid (a little more saturation and contrast) or a LUT of your own from
    `Mods/ColorGrading/lut.png` (a 1024x32 strip, as ReShade's and Unity's LUTs; 256x16 and 4096x64 work too). The
    first start without one writes `neutral.png` there, a LUT that changes nothing: grade a screenshot in an image
    editor, apply the same adjustments to `neutral.png` and save it as `lut.png`.
  - **Vignette:** darkens the picture's corners (measured in a 16:9 frame, so ultrawide keeps its middle bright).
- **Low-end GPUs:** weak integrated graphics start at a lower internal resolution on first launch.

## Controls

- **Remappable keyboard, mouse and controller** bindings, in the launcher's settings and in Sly 2's own Controls page.
- **Advanced Input Mapping (Sly 2)** is an optional mode for binding keyboard, mouse and controller inputs to
  individual actions, including character moves, gadgets, vehicles and minigames.
- **Mouse camera and aim** with their own sensitivities.
- **Per-view invert options (Sly 2):** invert the camera, the binocucom and the turrets separately, for the controller
  and for the mouse.
- **Controller prompts:** PlayStation (PS2 / PS4 / PS5), Xbox, Nintendo, Steam Deck, Steam Controller and keyboard
  icons, following the device you use, or the games' original PS2 icons on every device.
- **Controller vibration** with the original rumble patterns, on Xbox, PlayStation, Steam Deck and other SDL
  controllers. **Settings > Controls > Controller vibration** turns it off for every game.
- **Menus with the mouse (Sly 2):** hover and click in the gadget grid, Job Help and the episode menus.

## Gameplay additions (Sly 2)

- **Replay Jobs:** replay any finished job from the episode menu. Multi-part **operations** are listed as a whole and
  play their parts in sequence, as in the story; each part can also be played alone.
- **30 save slots:** ten pages of three.
- **Skippable cutscenes** where the original paused the game for a scene.
- **Ancestor techniques from the Thievius Raccoonus** (after *Sly Cooper: Thieves in Time*): four techniques for
  Sly, each on a page of the Thievius Raccoonus hidden in an episode's vault (collect its 30 clue bottles and open
  it). Found pages appear in ThiefNet's new third page, **Thievius Raccoonus** (Square, or the stick right from
  Sell). Choosing a found page starts its **training**: ThiefNet closes and Sly has the technique for a challenge
  against the clock in the hub (a panel shows the goal, the progress and the time); done in time, he has learned
  it, otherwise it can be tried again from ThiefNet. Saves whose vaults are already open have their pages.
  | Technique | Training |
  | --- | --- |
  | Rail Sprint | 1500 units along ropes and rails holding R1, in 60 s |
  | Silent Sprint | 15 s of sprinting near guards without alerting them, in 120 s |
  | Leaping Dragon | three leaps landed on spires, in 90 s |
  | Climbing Cobra | 1000 units up or down poles and pipes holding R1, in 45 s |

  - **Rail Sprint** ("Tennessee Kid" Cooper, Episode 1's vault): hold R1 on ropes and rails to run 1.75 times as fast.
  - **Silent Sprint** (Rioichi Cooper, Episode 2): the sprint (R1) makes no noise for the guards, and Sly leaves dark
    shadow afterimages behind him (drawn on Windows and Linux).
  - **Leaping Dragon** (Rioichi Cooper, Episode 3): on a spire, hold Circle: the spire you point the stick (or the
    camera) at, up to about five times Sly's own spire jumps away, is marked; let go and Sly leaps across on a long
    arc and lands perched on it.
  - **Climbing Cobra** (Salim al-Kupar, Episode 4): hold R1 on poles and pipes to climb 1.8 times as fast.
  What Sly has learned is kept in a small `saveN.pc` file beside the save slot, so the save itself stays the PS2's
  format.

## Mods

- **A Mods folder** for Sly 1 and Sly 2 that replaces textures, music, dialog, text and movies without changing the
  installed game.
- **Texture packs** and **the PS3 HD textures:** import the redrawn textures of *The Sly Collection* (PS3) from your
  own copy, then turn them on or off in the settings.
- **Texture dump at install** (Sly 2) so you have every original texture to start from.

The guide: [MODDING.md](MODDING.md).

## The debug menu and PC Extras

With **Settings > Advanced > Debug mode** on, Sly 2 has a debug menu (F10, or R1 + Select): warps to every world,
character switching, noclip, teleports, job tools, game speed, a free camera and a frame-rate display. Its **PC Extras**
let you play as **Carmelita** (with her own moves), as the game's **guards and bosses**, or fly **Bentley's RC chopper**
in any world. Sly 1 has its own debug window (F3).

Details and controls: [DEBUG-MENU.md](DEBUG-MENU.md).
