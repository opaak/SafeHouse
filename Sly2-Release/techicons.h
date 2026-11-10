#pragma once

// PC addition: the icons of the Thievius Raccoonus techniques (raccoonus.h), drawn in ThiefNet's Thievius Raccoonus
// page, the training's panel and the messages. Each by its key (leaping_dragon, silent_sprint, rail_sprint,
// climbing_cobra): the Mods folder's Icons/<key>.png when there (any square size up to 1024, straight alpha), else the
// built-in one (assets/icons, embedded). Loaded once a session, at first use.

// The icon's GL texture (0: none), uploaded top row first with mipmaps (CHud::DrawTexQuad's layout)
unsigned TexTechniqueIcon(const char* szKey);
// Drawn on the HUD, centred at (cx, cy) canvas pixels, h high (square on the picture); rgb in GS units (0x80 as is,
// less dims it), alpha 0..128; z the depth (0..1)
void DrawTechniqueIcon(const char* szKey, float cx, float cy, float h, float rgb, float alpha, float z);
