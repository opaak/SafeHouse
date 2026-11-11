// See techicons.h.

#include "techicons.h"

#include "disc.h"
#include "gl.h"
#include "hud.h"
#include "../platform/platform.h"

#include "techicon_leaping_dragon.h"
#include "techicon_silent_sprint.h"
#include "techicon_rail_sprint.h"
#include "techicon_climbing_cobra.h"
#include "techicon_meter_recharger.h"
#include "techicon_flash_bomb.h"
#include "techicon_run_while_charging.h"
#include "techicon_moving_pickpocket.h"
#include "techicon_homing_darts.h"
#include "techicon_adrenaline_burst_plus.h"
#include "techicon_light_as_a_feather.h"
#include "techicon_rocket_boots.h"
#include "techicon_bomb_blast_2.h"
#include "techicon_mega_stomp.h"
#include "techicon_mega_charge_attack.h"
#include "techicon_scorching_hover_pack.h"
#include "techicon_knights_leap.h"
#include "techicon_treasure_sense.h"
#include "techicon_shadow_cloak.h"
#include "techicon_coin_magnet.h"

// (stb_image's functions compiled into this file, static: as texmod.cpp and qolview.cpp, so no clash)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
struct BUILTIN { const char* szKey; const unsigned char* pb; size_t cb; };
const BUILTIN s_abuiltin[] = {
    {"leaping_dragon", techicon_leaping_dragon_png, sizeof(techicon_leaping_dragon_png)},
    {"silent_sprint", techicon_silent_sprint_png, sizeof(techicon_silent_sprint_png)},
    {"rail_sprint", techicon_rail_sprint_png, sizeof(techicon_rail_sprint_png)},
    {"climbing_cobra", techicon_climbing_cobra_png, sizeof(techicon_climbing_cobra_png)},
    {"meter_recharger", techicon_meter_recharger_png, sizeof(techicon_meter_recharger_png)},
    {"flash_bomb", techicon_flash_bomb_png, sizeof(techicon_flash_bomb_png)},
    {"run_while_charging", techicon_run_while_charging_png, sizeof(techicon_run_while_charging_png)},
    {"moving_pickpocket", techicon_moving_pickpocket_png, sizeof(techicon_moving_pickpocket_png)},
    {"homing_darts", techicon_homing_darts_png, sizeof(techicon_homing_darts_png)},
    {"adrenaline_burst_plus", techicon_adrenaline_burst_plus_png, sizeof(techicon_adrenaline_burst_plus_png)},
    {"light_as_a_feather", techicon_light_as_a_feather_png, sizeof(techicon_light_as_a_feather_png)},
    {"rocket_boots", techicon_rocket_boots_png, sizeof(techicon_rocket_boots_png)},
    {"bomb_blast_2", techicon_bomb_blast_2_png, sizeof(techicon_bomb_blast_2_png)},
    {"mega_stomp", techicon_mega_stomp_png, sizeof(techicon_mega_stomp_png)},
    {"mega_charge_attack", techicon_mega_charge_attack_png, sizeof(techicon_mega_charge_attack_png)},
    {"scorching_hover_pack", techicon_scorching_hover_pack_png, sizeof(techicon_scorching_hover_pack_png)},
    {"knights_leap", techicon_knights_leap_png, sizeof(techicon_knights_leap_png)},
    {"treasure_sense", techicon_treasure_sense_png, sizeof(techicon_treasure_sense_png)},
    {"shadow_cloak", techicon_shadow_cloak_png, sizeof(techicon_shadow_cloak_png)},
    {"coin_magnet", techicon_coin_magnet_png, sizeof(techicon_coin_magnet_png)},
};

std::map<std::string, unsigned> s_mptex;   // (0: tried, none)

bool FPrint() { static const bool s_f = std::getenv("SLY2N_ICONPRINT") != nullptr; return s_f; }

unsigned TexFromPng(const unsigned char* pb, size_t cb, const std::string& what)
{
    int dx = 0, dy = 0;
    stbi_uc* p = stbi_load_from_memory(pb, int(cb), &dx, &dy, nullptr, 4);
    if (!p) { std::fprintf(stderr, "icons: %s: not a PNG this reads (%s)\n", what.c_str(), stbi_failure_reason()); return 0; }
    if (dx <= 0 || dy <= 0 || dx > 1024 || dy > 1024)
    {
        std::fprintf(stderr, "icons: %s: %d x %d (at most 1024 a side)\n", what.c_str(), dx, dy);
        stbi_image_free(p);
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(p);
    if (FPrint()) std::printf("icons: %s (%d x %d)\n", what.c_str(), dx, dy);
    return tex;
}
}  // namespace

unsigned TexTechniqueIcon(const char* szKey)
{
    if (!szKey || !*szKey) return 0;
    const auto it = s_mptex.find(szKey);
    if (it != s_mptex.end()) return it->second;
    unsigned tex = 0;
    // the Mods folder's own first
    if (!g_disc.DirMods().empty())
    {
        const std::filesystem::path path = cane::path(g_disc.DirMods()) / "Icons" / (std::string(szKey) + ".png");
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec))
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<unsigned char> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            tex = TexFromPng(ab.data(), ab.size(), cane::utf8(path));
        }
    }
    if (!tex)
        for (const BUILTIN& b : s_abuiltin)
            if (!std::strcmp(b.szKey, szKey)) tex = TexFromPng(b.pb, b.cb, std::string("built-in ") + szKey);
    s_mptex[szKey] = tex;
    return tex;
}

void DrawTechniqueIcon(const char* szKey, float cx, float cy, float h, float rgb, float alpha, float z)
{
    const unsigned tex = TexTechniqueIcon(szKey);
    if (!tex) return;
    const float hx = 0.5f * h * (480.0f / 448.0f);   // (the canvas's pixel aspect, as the prompt icons)
    // (uploaded top row first: v1 the top edge, v0 the bottom: button_prompts.h's layout)
    g_hud.DrawTexQuad(tex, cx - hx, cy - 0.5f * h, cx + hx, cy + 0.5f * h, 0.0f, 1.0f, 1.0f, 0.0f, glm::vec4(rgb, rgb, rgb, alpha), z);
}
