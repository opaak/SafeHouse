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
};
// "&2O&." is the circle button's icon in the game's texts (shown as the device's own: pcinput.h)
const PCITEM s_aitem[] = {
    {kItemLeapingDragon, "leapingDragon", 7, 1200, 2, "Leaping Dragon",
     "Rioichi Cooper's technique. On a spire, hold &2O&. to target a distant spire, then let go to leap onto it"},
};

const PCITEM* PitemOf(int id)
{
    for (const PCITEM& it : s_aitem)
        if (it.id == id) return &it;
    return nullptr;
}

bool s_afOwned[sizeof(s_aitem) / sizeof(s_aitem[0])] = {};

bool FPrint() { static const bool s_f = std::getenv("SLY2N_PCPROGRESSPRINT") != nullptr; return s_f; }

std::filesystem::path PathPc(const std::string& pathSlot) { return cane::path(pathSlot + ".pc"); }
}  // namespace

bool FPcItem(int id) { return PitemOf(id) != nullptr; }

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

void ResetPcProgress()
{
    for (bool& f : s_afOwned) f = false;
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
