// See raccoonus.h.

#include "raccoonus.h"

#include "clock.h"
#include "hud.h"
#include "pcprogress.h"
#include "ref.h"
#include "savedata.h"
#include "screenrt.h"
#include "shop.h"
#include "sound.h"
#include "sw.h"
#include "transition.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp
int IShopPage();   // shop.cpp

namespace
{
// The ancestors after Sly 4's: Tennessee Kid Cooper made the rail walk his escape; Rioichi Cooper his silent sprint and
// the Leaping Dragon from the spires he perfected; Salim al-Kupar the Climbing Cobra.
const RACPAGE s_apage[] = {
    {kItemRailSprint, 1, "\"Tennessee Kid\" Cooper", "Rail Sprint", "Hold &2R&. on ropes and rails to run along them"},
    {kItemSilentSprint, 2, "Rioichi Cooper", "Silent Sprint", "Sprint with &2R&. without a sound; Sly leaves only shadows behind"},
    {kItemLeapingDragon, 3, "Rioichi Cooper", "Leaping Dragon", "On a spire, hold &2O&. to mark a distant spire, let go to leap onto it"},
    {kItemClimbingCobra, 4, "Salim al-Kupar", "Climbing Cobra", "Hold &2R&. on poles and pipes to climb them at speed"},
};
constexpr int CPAGE = int(sizeof(s_apage) / sizeof(s_apage[0]));

int s_isel = 0;

// the message over the HUD (a page found / learned): its two lines and when it goes
std::string s_strMsg1, s_strMsg2;
float s_tMsgEnd = -1.0f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_RACPRINT") != nullptr; return s_f; }

bool FRefTrue(const CRef& r) { return (r.m_tagk == TAGK_Bool && r.m_bool) || (r.m_tagk == TAGK_S32 && r.m_n); }

int IepCurrent()
{
    const auto& awrec = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    return i >= 0 && i < int(awrec.size()) ? awrec[size_t(i)].slot : -1;
}

void ShowMsg(const std::string& s1, const std::string& s2, float dt)
{
    s_strMsg1 = s1;
    s_strMsg2 = s2;
    s_tMsgEnd = g_clock.tReal + dt;
}

// canvas pixels (640 x 448) to DrawTextBox's UI units, as debugmenu.cpp's DrawLine
constexpr float DEPTH = 16760000.0f;   // (over the HUD's screen objects, as the debug menu)
void DrawLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float x, float y, float size, glm::vec4 off)
{
    g_hud.DrawTextBox(font, picons, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
float DxLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size) { return DxTextUi(font, picons, text, size / 51.2f); }
}  // namespace

int CRaccoonusPages() { return CPAGE; }
const RACPAGE& RacPage(int i) { return s_apage[std::clamp(i, 0, CPAGE - 1)]; }

bool FRaccoonusPageFound(int i)
{
    if (i < 0 || i >= CPAGE) return false;
    const int iep = s_apage[i].iep;
    const int16_t symVault = static_cast<int16_t>(NSymProgress("fVault"));
    for (const WREC& w : AwrecWorlds())
    {
        if (w.slot != iep || w.an.empty()) continue;
        std::vector<int16_t> akey(w.an.begin(), w.an.end());
        akey.push_back(symVault);
        if (FRefTrue(RefGetProgressKey(akey))) return true;
    }
    return false;
}

bool FRaccoonusPageLearned(int i) { return i >= 0 && i < CPAGE && FPcItemOwned(s_apage[i].item); }

void NoteVaultOpened()
{
    const int iep = IepCurrent();
    for (int i = 0; i < CPAGE; ++i)
        if (s_apage[i].iep == iep)
        {
            if (FPrint()) std::printf("raccoonus: episode %d's vault: the page of %s\n", iep, s_apage[i].szTechnique);
            ShowMsg(std::string("A page of the Thievius Raccoonus! ") + s_apage[i].szAncestor + ": " + s_apage[i].szTechnique,
                    "Study it in ThiefNet's Thievius Raccoonus.", 8.0f);
            return;
        }
}

void ResetRaccoonusPage() { s_isel = std::clamp(s_isel, 0, CPAGE - 1); }

void StepRaccoonusPage(int d)
{
    const int i = s_isel + d;
    if (i < 0 || i >= CPAGE)
    {
        PsndStartSound(0x36);   // (the shop's end of the list)
        return;
    }
    s_isel = i;
    PsndStartSound(0x35);   // (the shop's step)
}

bool FConfirmRaccoonusPage()
{
    const int i = s_isel;
    if (!FRaccoonusPageFound(i) || FRaccoonusPageLearned(i))
    {
        PsndStartSound(0x36);
        return false;
    }
    // [phase 1: studying the page learns it; the training challenge comes here]
    SetPcItemOwned(s_apage[i].item, true);
    PsndStartSound(0x29);   // (the shop's purchase)
    ShowMsg(std::string("Sly learned ") + s_apage[i].szTechnique + "!", s_apage[i].szHow, 6.0f);
    if (FPrint()) std::printf("raccoonus: %s learned\n", s_apage[i].szTechnique);
    return true;
}

std::string StrRaccoonusDescription()
{
    const RACPAGE& p = s_apage[std::clamp(s_isel, 0, CPAGE - 1)];
    if (!FRaccoonusPageFound(s_isel))
        return std::string("A missing page. Episode ") + std::to_string(p.iep) +
               ": collect its 30 clue bottles and open the vault to find it.";
    std::string s = std::string(p.szTechnique) + " (" + p.szAncestor + "): " + p.szHow + ".";
    if (!FRaccoonusPageLearned(s_isel)) s += " Press &2X&. to study it.";
    return s;
}

void DrawRaccoonusHud(int width, int height)
{
    const bool fPage = FShopShown() && IShopPage() == 2;
    const bool fMsg = g_clock.tReal < s_tMsgEnd;
    if (!fPage && !fMsg) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    g_hud.Begin();
    const float z = DEPTH / 16777216.0f;
    if (fPage)
    {
        // a page of the book: parchment over the list's place, the four techniques in rows
        constexpr float x0 = 128.0f, x1 = 512.0f, y0 = 104.0f, dyRow = 34.0f, size = 17.0f;
        const float y1 = y0 + 40.0f + dyRow * float(CPAGE) + 8.0f;
        g_hud.FillScreenRect(x0 - 3.0f, y0 - 3.0f, x1 + 3.0f, y1 + 3.0f, glm::vec4(30, 18, 8, 128), z - 3e-4f);
        g_hud.FillScreenRect(x0, y0, x1, y1, glm::vec4(118, 100, 66, 128), z - 2e-4f);
        const glm::vec4 offTitle(70, 30, 8, 0), offText(30, 20, 10, 0), offDim(70, 60, 44, 0), offLearned(18, 60, 18, 0),
            offFound(90, 50, 8, 0);
        const std::string strTitle = "THIEVIUS RACCOONUS";
        DrawLine(*pfont, picons, strTitle, (x0 + x1) * 0.5f - DxLine(*pfont, picons, strTitle, 20.0f) * 0.5f, y0 + 8.0f, 20.0f, offTitle);
        for (int i = 0; i < CPAGE; ++i)
        {
            const float y = y0 + 40.0f + dyRow * float(i);
            const bool fFound = FRaccoonusPageFound(i), fLearned = FRaccoonusPageLearned(i);
            if (i == s_isel) g_hud.FillScreenRect(x0 + 6.0f, y - 3.0f, x1 - 6.0f, y + dyRow - 6.0f, glm::vec4(150, 132, 92, 128), z - 1e-4f);
            const RACPAGE& p = s_apage[i];
            std::string strName = fFound ? std::string(p.szTechnique) : std::string("? ? ?");
            DrawLine(*pfont, picons, strName, x0 + 16.0f, y, size, fFound ? offText : offDim);
            DrawLine(*pfont, picons, fFound ? std::string(p.szAncestor) : std::string("Episode ") + std::to_string(p.iep), x0 + 16.0f,
                     y + 15.0f, size * 0.65f, offDim);
            const std::string strState = fLearned ? "Learned" : fFound ? "Found: study it" : "Missing";
            DrawLine(*pfont, picons, strState, x1 - 16.0f - DxLine(*pfont, picons, strState, size * 0.8f), y + 3.0f, size * 0.8f,
                     fLearned ? offLearned : fFound ? offFound : offDim);
        }
    }
    if (fMsg)
    {
        // a band at the top of the picture, fading out over its last second
        const float u = std::clamp(s_tMsgEnd - g_clock.tReal, 0.0f, 1.0f);
        const float a = 128.0f * u;
        g_hud.FillScreenRect(60.0f, 26.0f, 580.0f, 76.0f, glm::vec4(0, 0, 0, 90.0f * u), z - 1e-4f);
        const glm::vec4 off1(110 * u, 90 * u, 40 * u, 0), off2(90 * u, 90 * u, 90 * u, 0);
        (void)a;
        DrawLine(*pfont, picons, s_strMsg1, 320.0f - DxLine(*pfont, picons, s_strMsg1, 17.0f) * 0.5f, 32.0f, 17.0f, off1);
        DrawLine(*pfont, picons, s_strMsg2, 320.0f - DxLine(*pfont, picons, s_strMsg2, 14.0f) * 0.5f, 54.0f, 14.0f, off2);
    }
    g_hud.Flush(width, height);
}
