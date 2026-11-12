#pragma once

// PC modding (the debug menu's Texture picker: "Export the picked model"): an object's model written as glTF
// binary (.glb) for Blender. Each group the frame showed is a node ("glob_<i>") with its triangles in its own
// space, its parts primitives with their textures (embedded PNGs, as the texture export writes them) and
// their blending (alphaMode); a root node turns the game's Z-up units (1 unit = 1 cm) into glTF's Y-up metres.
// A skinned group is written in the pose it was in (its bones applied). The nodes' extras note the game's own
// numbers (sly2_glob: the group's index; sly2_posed) for the import (a later step) to map the edits back.

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLOBSET;

struct MODELEXPORT
{
    std::shared_ptr<GLOBSET> pglobset;              // the object's model
    std::vector<glm::mat4> amatGlob;                // per group its matrix this frame
    std::vector<std::vector<glm::mat4>> aamatBone;  // per skinned group its bones this frame
    std::vector<uint8_t> afVisible;                 // per group whether the frame showed it
    int oid = -1;
    // (optional) the frame of reference: the file's origin (the group picked, at its middle); else the first shown
    // rigid group's matrix
    bool fRef = false;
    glm::mat4 matRef{1.0f};
};

// The model written to path (the folders made); false with *perr on failure, else *pstrInfo a short summary
bool FExportModelGlb(const MODELEXPORT& mx, const std::string& name, const std::string& path, std::string* pstrInfo, std::string* perr);
