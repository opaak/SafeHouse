#pragma once

// PC modding: model mods (Mods/Models/*.glb or *.gltf), the other half of modelexport.h. A file exported from the
// game (the debug menu's Texture picker), edited in Blender and saved back (glTF 2.0; "Include > Custom Properties"
// keeps the game's notes, but the names alone do) replaces, in its world and object, the groups it was exported
// with:
//   - each mesh object glob_<n> (or with the custom property sly2_glob) becomes group n's triangles; other meshes
//     join the group picked at the export (sly2_pick_glob, or the file name's _g<n>), placed where they are in Blender;
//   - a group exported but deleted in Blender is gone; a group not in the file stays as it was;
//   - each mesh's material named sly2_g<g>_s<s> draws with that part's shader (texture, blending, animation), any
//     other material with the group's first part's (the textures themselves change through Mods/Textures);
//   - the light (the vertex colours) is taken from the nearest original vertex, so it sits in the level as before.
// The file is matched by its world and object: the custom properties sly2_world / sly2_oid, or the file name
// w<world>_<class>_oid<oid>... as exported. Skinned groups (bones), instances (a template's copies) and warped
// groups are left as they are. Applied when the world's groups are first made for the GPU (render.cpp
// BuildSceneGpu): change a file, then enter the world again.

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLOBSET;

// (props.h) A glTF file's triangles as the game's: every mesh of its scene placed where it is (Y-up metres turned into
// the game's Z-up centimetres), its UVs as the game's, its material's picture (game texels, as model mods take them)
// and blending. fGameExport: the file was exported from the game (its root has sly2_oid).
struct GLTFIMAGE
{
    int dx = 0, dy = 0;
    std::shared_ptr<const std::vector<uint8_t>> pab;   // RGBA, rows bottom-up, alpha 0..0x80
    std::string name;                                  // its texture-mod name
    bool fClampS = false, fClampT = false;
    bool fPlain = false;                               // a plain colour (one texel)
};
struct GLTFPRIM
{
    std::vector<glm::vec3> apos, anrm;
    std::vector<glm::vec2> auv;
    std::vector<uint32_t> aidx;
    std::shared_ptr<GLTFIMAGE> pimg;   // null: no material picture (white)
    int alphaMode = 0;                 // 0 opaque, 1 mask (cut-out), 2 blend
    // per vertex its colour (1 = the game's 0x80): the export's _COLOR_GAME, else COLOR_0; empty: none
    std::vector<glm::vec4> acol;
    bool fColGame = false;             // (the game's own: its baked light in them; else painted, lit at its place)
    glm::vec3 rgbFactor{1.0f};         // the material's baseColorFactor, as the game's (gamma) scale
};
bool FReadGltfPrims(const std::string& path, std::vector<GLTFPRIM>& aprim, bool* pfGameExport, std::string& err);

// Before a build of the world's groups: the files read again when the world or the folder changed (a file edited and
// the world entered again shows the edit)
void BeginModelMods();
// The world's model mods applied to this object's model (oid in the current world), once per model
void ApplyModelMods(GLOBSET& gs, int oid);
