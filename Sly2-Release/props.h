#pragma once

// PC modding: props, models of your own placed in a world (Mods/Props). A model is a glTF file in Mods/Props/Models
// (made in Blender, its origin where it stands; or exported from the game, which is stood on its base); a world's
// props are the lines of Mods/Props/w<world>.txt:
//     <model file> <x> <y> <z> <turn> [scale <s>] [tilt <pitch> <roll>] [after <episode 1..8>]
// (the game's units, centimetres, Z up; turn about Z in degrees; "after 3": shown once episode 3 is complete).
// Each is drawn as one of the frame's extra draws (render.h), lit by the level's light where it stands (the nearest
// level vertices' colours, shaded by its own normals): cel shading, outlines and the ambient occlusion as the level's.
// Not solid (Sly passes through) and not in the level's shadow maps. The debug menu's Display > Graphics > Props places,
// turns, scales, raises and saves them (render.h FCrosshairHit: the point under the crosshair).

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct SCENE;
struct EXTRADRAW;

// the frame's props (render.cpp's extra draws): loaded for the world, made for the GPU when first drawn
void CollectPropDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra);

// ---- the placement tool (debugmenu.cpp)
int CPropModels();                          // Mods/Props/Models' files (read again by RescanPropModels)
void RescanPropModels();
std::string StrPropModel(int imodel);
int CProps();                               // this world's props
int IpropSelected();                        // -1 none
void SelectProp(int iprop);
void SelectPropNearest(const glm::vec3& pos);
std::string StrPropSelected();              // a line for the menu: its model, place, turn, scale, condition
bool FPlaceProp(int imodel, const glm::vec3& pos, float turnDeg);   // a new prop, selected
void MovePropSelected(const glm::vec3& pos);
void TurnPropSelected(float dDeg);
void ScalePropSelected(float r);
void RaisePropSelected(float dz);
void CyclePropSelectedAfter();              // shown: always, after episode 1 .. 8
void DeletePropSelected();
bool FSaveProps(std::string* pstr);         // Mods/Props/w<world>.txt
