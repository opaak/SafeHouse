// See pcprogress.h.

#include "pcprogress.h"

#include "../platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
struct PCITEM
{
    int id;
    const char* szKey;    // the file's key
    int kind, cost, episode;
    const char* szName;
    const char* szDesc;
    int use = -1;         // NPcItemUse: -1 passive, 1 held, 2 at once
    int power = 0;        // NPcItemPower
    bool fSold = false;   // FPcItemSold: in ThiefNet for coins
};
// "&2O&." is the circle button's icon in the game's texts (shown as the device's own: pcinput.h)
const PCITEM s_aitem[] = {
    {kItemRailSprint, "railSprint", 7, 700, 1, "Rail Sprint",
     "Hold &2R&. on ropes and rails to run along them"},
    {kItemSilentSprint, "silentSprint", 7, 900, 1, "Silent Sprint",
     "Rioichi Cooper's technique. Sprinting with &2R&. makes no sound, and Sly leaves only shadows behind"},
    {kItemLeapingDragon, "leapingDragon", 7, 1200, 2, "Leaping Dragon",
     "Rioichi Cooper's technique. On a spire, hold &2O&. to target a distant spire, then let go to leap onto it"},
    {kItemClimbingCobra, "climbingCobra", 7, 800, 3, "Climbing Cobra",
     "Salim al-Kupar's technique. Hold &2R&. on poles and pipes to climb them at speed"},
    // the extra powerups (extrapowerups.h); their use and power here, what they do there
    {kItemMeterRechargerSly, "meterRechargerSly", 7, 1500, 3, "Meter Recharger",
     "From Sly 4. When the gadget meter runs empty, it fills up again at once; then it needs 60 s to recharge", -1, 0, true},
    {kItemMeterRechargerBentley, "meterRechargerBentley", 9, 1500, 3, "Meter Recharger",
     "From Sly 4. When the gadget meter runs empty, it fills up again at once; then it needs 60 s to recharge", -1, 0, true},
    {kItemMeterRechargerMurray, "meterRechargerMurray", 8, 1500, 3, "Meter Recharger",
     "From Sly 4. When the gadget meter runs empty, it fills up again at once; then it needs 60 s to recharge", -1, 0, true},
    // Sly 4's passive upgrades, in ThiefNet as the story goes on (always on once bought)
    {kItemMovingPickpocket, "movingPickpocket", 7, 500, 2, "Moving Pickpocket",
     "From Sly 4. Hold circle while picking a pocket to keep reaching and follow the guard. Let go to take the loot", -1, 0, true},
    {kItemRunWhileCharging, "runWhileCharging", 7, 700, 2, "Run While Charging",
     "From Sly 4. Sly can walk while he winds up a charged cane swing, holding &2S&.", -1, 0, true},
    {kItemHomingDarts, "homingDarts", 9, 900, 3, "Homing Darts",
     "From Sly 4. Bentley's darts lock onto the guard nearest the crosshair", -1, 0, true},
    {kItemLightAsAFeather, "lightAsAFeather", 8, 900, 3, "Light as a Feather",
     "From Sly 4. Murray can run with &2R&. while he carries something, and runs faster with it", -1, 0, true},
    {kItemFlashBomb, "flashBomb", 7, 1400, 4, "Flash Bomb",
     "From Sly 4. The Smoke Bomb also flashes: the guards near it are stunned, and the alarm stays down twice as long", -1, 0, true},
    {kItemAdrenalineBurstPlus, "adrenalineBurstPlus", 9, 1400, 5, "Adrenaline Burst Plus",
     "From Sly 4. Adrenaline Burst runs 1.4 times as fast", -1, 0, true},
    {kItemRocketBoots, "rocketBoots", 7, 1300, 4, "Rocket Boots",
     "From Sly 3. The Stealth Slide no longer slows down, uses half the gadget power, and still makes no sound", -1, 0, true},
    {kItemBombBlast2, "bombBlast2", 9, 1100, 4, "Bomb Blast v2.0",
     "From Sly 4. Bentley's bombs blast 1.5 times as wide", -1, 0, true},
    {kItemMegaChargeAttack, "megaChargeAttack", 7, 1600, 5, "Mega Charge Attack",
     "From Sly 4. The charged cane strike hits every guard around Sly, harder, and knocks them back", -1, 0, true},
    {kItemMegaStomp, "megaStomp", 8, 1500, 5, "Mega Stomp",
     "From Sly 4. The Thunder Flop's landing sends out a shockwave that knocks down the guards around Murray", -1, 0, true},
    {kItemScorchingHoverPack, "scorchingHoverPack", 9, 1800, 6, "Scorching Hover Pack",
     "From Sly 4. The Hover Pack's jet burns the guards below Bentley as he hovers", -1, 0, true},
};

const PCITEM* PitemOf(int id)
{
    for (const PCITEM& it : s_aitem)
        if (it.id == id) return &it;
    return nullptr;
}

bool s_afOwned[sizeof(s_aitem) / sizeof(s_aitem[0])] = {};
// the PC items on the buttons (IdPcSlot): [character][button]
int s_aidSlot[3][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};
const char* const s_aszSlotChar[3] = {"sly", "bentley", "murray"};
std::string StrSlotKey(int ichar, int j) { return std::string("slot.") + s_aszSlotChar[ichar] + "." + std::to_string(j); }

bool FPrint() { static const bool s_f = std::getenv("SLY2N_PCPROGRESSPRINT") != nullptr; return s_f; }

std::filesystem::path PathPc(const std::string& pathSlot) { return cane::path(pathSlot + ".pc"); }
}  // namespace

bool FPcItem(int id) { return PitemOf(id) != nullptr; }
int CPcItems() { return int(sizeof(s_aitem) / sizeof(s_aitem[0])); }
int IdPcItem(int i) { return i >= 0 && i < CPcItems() ? s_aitem[i].id : -1; }

bool FPcItemOwned(int id)
{
    const PCITEM* p = PitemOf(id);
    return p && s_afOwned[p - s_aitem];
}

void SetPcItemOwned(int id, bool fOwned)
{
    if (const PCITEM* p = PitemOf(id)) s_afOwned[p - s_aitem] = fOwned;
}

int NPcItemKind(int id) { const PCITEM* p = PitemOf(id); return p ? p->kind : -1; }
int NPcItemCost(int id) { const PCITEM* p = PitemOf(id); return p ? p->cost : 0; }
int NPcItemEpisode(int id) { const PCITEM* p = PitemOf(id); return p ? p->episode : -1; }
const char* SzPcItemName(int id) { const PCITEM* p = PitemOf(id); return p ? p->szName : ""; }
const char* SzPcItemDesc(int id) { const PCITEM* p = PitemOf(id); return p ? p->szDesc : ""; }
int NPcItemUse(int id) { const PCITEM* p = PitemOf(id); return p ? p->use : -1; }
int NPcItemPower(int id) { const PCITEM* p = PitemOf(id); return p ? p->power : 0; }
bool FPcItemSold(int id) { const PCITEM* p = PitemOf(id); return p && p->fSold; }
const char* SzPcItemIcon(int id)
{
    switch (id)
    {
    case kItemLeapingDragon: return "leaping_dragon";
    case kItemSilentSprint: return "silent_sprint";
    case kItemRailSprint: return "rail_sprint";
    case kItemClimbingCobra: return "climbing_cobra";
    case kItemMeterRechargerSly:
    case kItemMeterRechargerBentley:
    case kItemMeterRechargerMurray: return "meter_recharger";
    case kItemFlashBomb: return "flash_bomb";
    case kItemRunWhileCharging: return "run_while_charging";
    case kItemMovingPickpocket: return "moving_pickpocket";
    case kItemHomingDarts: return "homing_darts";
    case kItemAdrenalineBurstPlus: return "adrenaline_burst_plus";
    case kItemLightAsAFeather: return "light_as_a_feather";
    case kItemRocketBoots: return "rocket_boots";
    case kItemBombBlast2: return "bomb_blast_2";
    case kItemMegaStomp: return "mega_stomp";
    case kItemMegaChargeAttack: return "mega_charge_attack";
    case kItemScorchingHoverPack: return "scorching_hover_pack";
    default: return nullptr;
    }
}

int IdPcSlot(int ichar, int j)
{
    const int id = ichar >= 0 && ichar < 3 && j >= 0 && j < 3 ? s_aidSlot[ichar][j] : -1;
    return id != -1 && NPcItemUse(id) == -1 ? -1 : id;   // (a passive one on a button from an older .pc: none)
}
void SetPcSlot(int ichar, int j, int id)
{
    if (ichar >= 0 && ichar < 3 && j >= 0 && j < 3) s_aidSlot[ichar][j] = FPcItem(id) ? id : -1;
}

void ResetPcProgress()
{
    for (bool& f : s_afOwned) f = false;
    for (auto& a : s_aidSlot)
        for (int& id : a) id = -1;
}

void LoadPcProgress(const std::string& pathSlot)
{
    ResetPcProgress();
    if (pathSlot.empty()) return;
    std::ifstream in(PathPc(pathSlot));
    if (!in) return;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        for (size_t i = 0; i < sizeof(s_aitem) / sizeof(s_aitem[0]); ++i)
            if (key == s_aitem[i].szKey) s_afOwned[i] = std::atoi(value.c_str()) != 0;
        for (int c = 0; c < 3; ++c)
            for (int j = 0; j < 3; ++j)
                if (key == StrSlotKey(c, j)) SetPcSlot(c, j, std::atoi(value.c_str()));
    }
    if (FPrint())
        for (size_t i = 0; i < sizeof(s_aitem) / sizeof(s_aitem[0]); ++i)
            std::printf("pc progress: %s %s = %d\n", pathSlot.c_str(), s_aitem[i].szKey, s_afOwned[i] ? 1 : 0);
}

bool FSavePcProgress(const std::string& pathSlot)
{
    if (pathSlot.empty()) return false;
    const std::filesystem::path path = PathPc(pathSlot);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "# SafeHouse: Sly 2's PC additions for this save slot (pcprogress.h)\n";
        for (size_t i = 0; i < sizeof(s_aitem) / sizeof(s_aitem[0]); ++i)
            out << s_aitem[i].szKey << '=' << (s_afOwned[i] ? 1 : 0) << '\n';
        for (int c = 0; c < 3; ++c)
            for (int j = 0; j < 3; ++j)
                if (s_aidSlot[c][j] != -1) out << StrSlotKey(c, j) << '=' << s_aidSlot[c][j] << '\n';
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (FPrint()) std::printf("pc progress: written %s (%s)\n", cane::utf8(path).c_str(), ec ? ec.message().c_str() : "ok");
    return !ec;
}
