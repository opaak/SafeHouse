#pragma once

// Texture mods (MODDING.md): Mods/Textures/<W>x<H>_<hash>.png replaces the texture whose
// decoded RGBA hashes to <hash> (FNV-1a 64), wherever the renderer's texture cache uploads it; --dump-textures and
// --export-textures write them. A replacement is used at its own resolution: the game's UVs are normalized, so a 4x PNG
// maps exactly like the original and shows 4x the detail. It gets a full mip chain (trilinear, anisotropic where the
// original is bilinear; point-sampled textures stay point-sampled when magnified).

#include <cstdint>
#include <string>
#include <vector>

// The texture's mod name: "<W>x<H>_<16 hex digits>.png".
std::string SzTextureModName(const std::vector<uint8_t>& rgba, int dx, int dy);
// --texture-pack DIR: a texture pack (DIR/Textures, the same names) under the mods folder's own textures, which win.
// The launcher passes the PS3 HD pack this way while it is turned on (ps3hd.cpp).
void SetTexturePackDirectory(const std::string& dir);
// (PC debug menu: the texture picker) The mods folder and the pack read again at their next use (the renderer's cache
// is reset with it: render.h ReloadTextureMods).
void RescanTextureMods();
// Where a texture's replacement comes from: 0 none, 1 the mods folder, 2 the texture pack.
int NTextureModSource(const std::string& name);
// The texture (the game's texels) written into the mods folder's Textures under its mod name, as the export writes it,
// for editing; *pfExisted when that file is there already (left as it is). False: no mods folder, or not written.
bool FCopyTextureToMods(const std::vector<uint8_t>& rgba, int dx, int dy, std::string* ppath, bool* pfExisted);
// The mods folder's PNG for this decoded texture as the game's texels (rows bottom-up, alpha in the PS2 range), at
// the PNG's own size (*pw x *ph); false: none, or no mods.
bool FLoadTextureMod(const std::vector<uint8_t>& rgba, int dx, int dy, std::vector<uint8_t>& out, int* pw, int* ph);
// A GL texture made from the mods folder's PNG for this decoded texture (with its mip chain), or 0 (none, or no mods).
unsigned GlTexFromMod(const std::vector<uint8_t>& rgba, int dx, int dy);
// True for a texture GlTexFromMod made (the renderer then samples it with the mipmapped samplers).
bool FTextureIsMod(unsigned tex);
// The renderer deleted it (render.cpp ResetTextureCache).
void ForgetModTexture(unsigned tex);
// --dump-textures DIR W: every texture of world W (a record index, boot, frontend) as a PNG named for the mods folder.
int DumpTexturesCommand(const std::string& dir, const std::string& world);
// Every world's textures (the boot world and every world record with a file) into DIR, a texture several worlds draw
// written once. fProgress: "textures I N WORLD" lines on stdout (cane-extractor, the launcher). Returns the count
// written, or -1.
int ExportAllTextures(const std::string& dir, bool fProgress);
// --export-textures without --convert: every world's textures into DIR (main.cpp: --out, else <installation>/Textures), written into
// DIR.partial and renamed when complete (an existing DIR is replaced).
int ExportTexturesCommand(const std::string& dir);
// A PNG (or any image stb_image reads) as RGBA8 rows, top row first; the path is UTF-8. False: missing or unreadable.
// (post.cpp: the colour-grading LUT)
bool FLoadImageRgba(const std::string& path, std::vector<uint8_t>& rgba, int* pw, int* ph);
// RGBA8 rows (top row first) written as a PNG at a UTF-8 path.
bool FWritePngRgba(const std::string& path, const std::vector<uint8_t>& rgba, int dx, int dy);
