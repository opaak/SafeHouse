// See raccoonus.h.

#include "raccoonus.h"

#include "actor.h"
#include "clock.h"
#include "guard.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "sound.h"
#include "hud.h"
#include "pcprogress.h"
#include "ref.h"
#include "region.h"
#include "savedata.h"
#include "screenrt.h"
#include "techicons.h"
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
// The trainings (see raccoonus.h): per page its goal (units of progress), time and text.
struct TRAINDEF { float goal; float tLimit; const char* szGoal; const char* szUnit; };
const TRAINDEF s_atrain[] = {
    {1500.0f, 60.0f, "Run along ropes and rails holding &2R&.", ""},
    {15.0f, 120.0f, "Sprint near guards without being noticed", " s"},
    {3.0f, 90.0f, "Leap from spire to spire", ""},
    {1000.0f, 45.0f, "Climb poles and pipes holding &2R&.", ""},
};
const RACPAGE s_apage[] = {
    {kItemRailSprint, 1, "\"Tennessee Kid\" Cooper", "Rail Sprint", "Hold &2R&. on ropes and rails to run along them", "rail_sprint"},
    {kItemSilentSprint, 2, "Rioichi Cooper", "Silent Sprint", "Sprint with &2R&. without a sound; Sly leaves only shadows behind", "silent_sprint"},
    {kItemLeapingDragon, 3, "Rioichi Cooper", "Leaping Dragon", "On a spire, hold &2O&. to mark a distant spire, let go to leap onto it", "leaping_dragon"},
    {kItemClimbingCobra, 4, "Salim al-Kupar", "Climbing Cobra", "Hold &2R&. on poles and pipes to climb them at speed", "climbing_cobra"},
};
constexpr int CPAGE = int(sizeof(s_apage) / sizeof(s_apage[0]));

int s_isel = 0;
float s_tPageShown = -10.0f;   // (the page's fade in)
static_assert(sizeof(s_atrain) / sizeof(s_atrain[0]) == sizeof(s_apage) / sizeof(s_apage[0]));

// The training under way (ipage -1: none)
struct TRAINING
{
    int ipage = -1;
    uint32_t serial = 0;    // its world's (another world ends it)
    float tStart = 0.0f;    // game time
    float progress = 0.0f;
    int cAlertStart = 0;    // the guards' alert at the start (Silent Sprint: more is a failure)
};
TRAINING s_train;

// the message over the HUD (a page found / learned): its two lines and when it goes
std::string s_strMsg1, s_strMsg2;
const char* s_szMsgIcon = nullptr;   // (techicons.h; null: none)
float s_tMsgEnd = -1.0f;

bool FPrint() { static const bool s_f = std::getenv("SLY2N_RACPRINT") != nullptr; return s_f; }

bool FRefTrue(const CRef& r) { return (r.m_tagk == TAGK_Bool && r.m_bool) || (r.m_tagk == TAGK_S32 && r.m_n); }

int IepCurrent()
{
    const auto& awrec = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    return i >= 0 && i < int(awrec.size()) ? awrec[size_t(i)].slot : -1;
}

void ShowMsg(const std::string& s1, const std::string& s2, float dt, const char* szIcon = nullptr)
{
    s_strMsg1 = s1;
    s_strMsg2 = s2;
    s_szMsgIcon = szIcon;
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

// the text's lines in w canvas pixels (broken at spaces)
std::vector<std::string> AstrWrapped(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size, float w)
{
    std::vector<std::string> astr;
    std::string line;
    size_t i = 0;
    while (i < text.size())
    {
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && DxLine(font, picons, next, size) > w)
        {
            astr.push_back(line);
            line = word;
        }
        else line = next;
        i = j + 1;
    }
    if (!line.empty()) astr.push_back(line);
    return astr;
}

// a frame of ThiefNet's look: a light blue border round a dark box
void FillFrame(float x0, float y0, float x1, float y1, float a, float z)
{
    g_hud.FillScreenRect(x0 - 2.0f, y0 - 2.0f, x1 + 2.0f, y1 + 2.0f, glm::vec4(16, 62, 84, a), z - 3e-4f);
    g_hud.FillScreenRect(x0, y0, x1, y1, glm::vec4(7, 11, 24, a), z - 2.5e-4f);
}
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
                    "Train it in ThiefNet's Thievius Raccoonus.", 8.0f, s_apage[i].szIcon);
            return;
        }
}

void ResetRaccoonusPage()
{
    s_isel = std::clamp(s_isel, 0, CPAGE - 1);
    s_tPageShown = g_clock.tReal;
}

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
    if (!FRaccoonusPageFound(i) || FRaccoonusPageLearned(i) || !g_psw)
    {
        PsndStartSound(0x36);
        return false;
    }
    s_train = TRAINING{};
    s_train.ipage = i;
    s_train.serial = g_psw->serial;
    s_train.tStart = g_clock.t;
    s_train.cAlertStart = g_sound.fModeAlert;
    PsndStartSound(0x29);   // (the shop's purchase)
    SetShopPage(-1);
    ShowMsg(std::string("Training: ") + s_apage[i].szTechnique, s_apage[i].szHow, 5.0f, s_apage[i].szIcon);
    if (FPrint()) std::printf("raccoonus: training %s\n", s_apage[i].szTechnique);
    return true;
}

bool FTrainingItem(int item) { return s_train.ipage >= 0 && s_apage[s_train.ipage].item == item && g_psw && s_train.serial == g_psw->serial; }

void NoteLeapLanded()
{
    if (s_train.ipage >= 0 && s_apage[s_train.ipage].item == kItemLeapingDragon) s_train.progress += 1.0f;
}

void UpdateRaccoonusTraining()
{
    if (s_train.ipage < 0) return;
    const RACPAGE& page = s_apage[s_train.ipage];
    const TRAINDEF& def = s_atrain[s_train.ipage];
    if (!g_psw || s_train.serial != g_psw->serial)
    {
        // (another world: the hub left, or Sly caught and the world reloaded)
        s_train = TRAINING{};
        ShowMsg(std::string("Training stopped: ") + page.szTechnique, "Try it again from ThiefNet's Thievius Raccoonus.", 5.0f);
        return;
    }
    const float dt = g_clock.dt;
    LO* plo = PloPoCurrent(g_psw);
    ACTOR* pa = plo ? PactorFromLo(plo) : nullptr;
    SLY* psly = pa ? PslyFromLo(pa->palo) : nullptr;
    PODATA* ppd = plo ? PpodataFromLo(plo) : nullptr;
    const uint32_t grfbtn = ppd && ppd->ppadctl ? ppd->ppadctl->grfbtn : 0u;
    constexpr uint32_t BTN_R1 = 0x8;
    if (psly && dt > 0.0f)
    {
        switch (page.item)
        {
        case kItemRailSprint:
            if (psly->state == 7 && psly->n4C8 == 3 && (grfbtn & BTN_R1)) s_train.progress += glm::length(glm::vec2(VelAloPos(psly->palo))) * dt;
            break;
        case kItemClimbingCobra:
            if (psly->state == 8 && (grfbtn & BTN_R1)) s_train.progress += std::fabs(psly->gCrvF50) * dt;
            break;
        case kItemSilentSprint:
        {
            if (g_sound.fModeAlert > s_train.cAlertStart)
            {
                s_train = TRAINING{};
                PsndStartSound(0x36);
                ShowMsg("Training failed: the guards noticed Sly.", "Try it again from ThiefNet's Thievius Raccoonus.", 6.0f);
                return;
            }
            s_train.cAlertStart = std::min(s_train.cAlertStart, g_sound.fModeAlert);   // (a chase from before that ended)
            if (psly->state != 1) break;
            const glm::vec3 pos = PosAloCenter(psly->palo);
            bool fNear = false;
            for (CID cid = 0x38; cid <= 0x43 && !fNear; ++cid)
                for (LO* pg : g_psw->mpcidaplo[cid])
                {
                    if (!pg->fInWorld || !FCidDerivedFrom(pg->Cid(), 0x30) || !PguardFromLo(pg)) continue;
                    if (glm::length(PosAloCenter(static_cast<ALO*>(pg)) - pos) < 900.0f) { fNear = true; break; }
                }
            if (fNear) s_train.progress += dt;
            break;
        }
        default: break;   // (Leaping Dragon: NoteLeapLanded)
        }
    }
    if (s_train.progress >= def.goal)
    {
        SetPcItemOwned(page.item, true);
        s_train = TRAINING{};
        PsndStartSound(0x29);
        ShowMsg(std::string("Sly learned ") + page.szTechnique + "!", page.szHow, 7.0f, page.szIcon);
        if (FPrint()) std::printf("raccoonus: %s learned\n", page.szTechnique);
        return;
    }
    if (g_clock.t - s_train.tStart > def.tLimit)
    {
        s_train = TRAINING{};
        PsndStartSound(0x36);
        ShowMsg(std::string("Training failed: out of time."), "Try it again from ThiefNet's Thievius Raccoonus.", 6.0f);
    }
}

std::string StrRaccoonusDescription()
{
    const RACPAGE& p = s_apage[std::clamp(s_isel, 0, CPAGE - 1)];
    if (!FRaccoonusPageFound(s_isel))
        return std::string("A missing page. Episode ") + std::to_string(p.iep) +
               ": collect its 30 clue bottles and open the vault to find it.";
    std::string s = std::string(p.szTechnique) + " (" + p.szAncestor + "): " + p.szHow + ".";
    if (!FRaccoonusPageLearned(s_isel))
    {
        const TRAINDEF& def = s_atrain[std::clamp(s_isel, 0, CPAGE - 1)];
        char ach[64];
        std::snprintf(ach, sizeof(ach), "%.0f%s in %.0f s", def.goal, def.szUnit, def.tLimit);
        s += std::string(" Press &2X&. to train: ") + def.szGoal + ", " + ach + ".";
    }
    return s;
}

void DrawRaccoonusHud(int width, int height)
{
    const bool fPage = FShopShown() && IShopPage() == 2;
    const bool fMsg = g_clock.tReal < s_tMsgEnd;
    const bool fTrain = s_train.ipage >= 0 && g_psw && s_train.serial == g_psw->serial && !FShopShown();
    if (!fPage && !fMsg && !fTrain) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    g_hud.Begin();
    const float z = DEPTH / 16777216.0f;
    if (fPage)
    {
        // ThiefNet's third page (shop.cpp hides its frame here): the tabs, the book's page with the four
        // techniques, the description and the buttons, over a backdrop as ThiefNet's
        const float u = std::clamp((g_clock.tReal - s_tPageShown) / 0.25f, 0.0f, 1.0f);
        const float a = 128.0f * u;
        g_hud.FillScreenRect(-480.0f, -40.0f, 1120.0f, 488.0f, glm::vec4(2, 3, 12, a), z - 4e-4f);
        const glm::vec4 offTab(22 * u, 38 * u, 58 * u, 0), offHead(34 * u, 62 * u, 84 * u, 0), offBlue(30 * u, 50 * u, 68 * u, 0);
        // the tabs: the other two pages dim, this one large on the right
        DrawLine(*pfont, picons, "Buy Gadgets", 52.0f, 36.0f, 17.0f, offTab);
        DrawLine(*pfont, picons, "Sell Loot", 52.0f + DxLine(*pfont, picons, "Buy Gadgets", 17.0f) + 18.0f, 36.0f, 17.0f, offTab);
        const std::string strTitle = "Thievius Raccoonus";
        DrawLine(*pfont, picons, strTitle, 590.0f - DxLine(*pfont, picons, strTitle, 26.0f), 28.0f, 26.0f, offHead);

        // the list's frame and in it the book's page: parchment, the techniques in rows
        FillFrame(50.0f, 64.0f, 590.0f, 306.0f, a, z);
        constexpr float x0 = 64.0f, x1 = 576.0f, y0 = 76.0f, dyRow = 54.0f, size = 20.0f;
        const float y1 = y0 + 8.0f + dyRow * float(CPAGE) + 2.0f;
        g_hud.FillScreenRect(x0 - 3.0f, y0 - 3.0f, x1 + 3.0f, y1 + 3.0f, glm::vec4(30, 18, 8, a), z - 2e-4f);
        g_hud.FillScreenRect(x0, y0, x1, y1, glm::vec4(118, 100, 66, a), z - 1.5e-4f);
        const glm::vec4 offText(30 * u, 20 * u, 10 * u, 0), offDim(70 * u, 60 * u, 44 * u, 0), offLearned(18 * u, 60 * u, 18 * u, 0),
            offFound(90 * u, 50 * u, 8 * u, 0);
        for (int i = 0; i < CPAGE; ++i)
        {
            const float y = y0 + 6.0f + dyRow * float(i);
            const bool fFound = FRaccoonusPageFound(i), fLearned = FRaccoonusPageLearned(i);
            if (i == s_isel) g_hud.FillScreenRect(x0 + 6.0f, y, x1 - 6.0f, y + dyRow - 4.0f, glm::vec4(150, 132, 92, a), z - 1e-4f);
            const RACPAGE& p = s_apage[i];
            const float yMid = y + (dyRow - 4.0f) * 0.5f;
            // its icon (a missing page's dark)
            DrawTechniqueIcon(p.szIcon, x0 + 34.0f, yMid, 42.0f, fFound ? 128.0f : 24.0f, (fFound ? 128.0f : 90.0f) * u, z);
            const std::string strName = fFound ? std::string(p.szTechnique) : std::string("? ? ?");
            DrawLine(*pfont, picons, strName, x0 + 68.0f, yMid - 19.0f, size, fFound ? offText : offDim);
            DrawLine(*pfont, picons, fFound ? std::string(p.szAncestor) : std::string("Episode ") + std::to_string(p.iep), x0 + 68.0f,
                     yMid + 2.0f, 14.0f, offDim);
            const std::string strState = fLearned ? "Learned" : fFound ? "Found: train" : "Missing";
            DrawLine(*pfont, picons, strState, x1 - 18.0f - DxLine(*pfont, picons, strState, 17.0f), yMid - 10.0f, 17.0f,
                     fLearned ? offLearned : fFound ? offFound : offDim);
        }

        // the description, as ThiefNet's box
        FillFrame(50.0f, 316.0f, 590.0f, 420.0f, a, z);
        DrawLine(*pfont, picons, "Description", 62.0f, 318.0f, 18.0f, offHead);
        g_hud.FillScreenRect(50.0f, 339.0f, 590.0f, 340.0f, glm::vec4(16, 62, 84, a), z - 2e-4f);
        const std::vector<std::string> astr = AstrWrapped(*pfont, picons, StrRaccoonusDescription(), 15.0f, 512.0f);
        for (size_t i = 0; i < astr.size() && i < 4; ++i)
            DrawLine(*pfont, picons, astr[i], 62.0f, 345.0f + 18.0f * float(i), 15.0f, offBlue);

        // the buttons
        const std::string strBtn = std::string(SzMenuConfirmGlyph()) + " Train    &2S&. Buy Gadgets    " + SzMenuBackGlyph() + " Exit";
        DrawLine(*pfont, picons, strBtn, 590.0f - DxLine(*pfont, picons, strBtn, 13.0f), 425.0f, 13.0f, offTab);
    }
    if (fTrain)
    {
        // the training's panel, top left: the technique, the goal, the progress bar, the time left
        const RACPAGE& page = s_apage[s_train.ipage];
        const TRAINDEF& def = s_atrain[s_train.ipage];
        constexpr float xPanel = 24.0f, y0 = 96.0f, h = 70.0f;
        g_hud.FillScreenRect(xPanel, y0, xPanel + 300.0f, y0 + h, glm::vec4(0, 0, 0, 88), z - 2e-4f);
        DrawTechniqueIcon(page.szIcon, xPanel + 32.0f, y0 + h * 0.5f, 56.0f, 128.0f, 128.0f, z);
        constexpr float x0 = xPanel + 64.0f, w = 236.0f;
        const float tLeft = std::max(0.0f, def.tLimit - (g_clock.t - s_train.tStart));
        char achTime[16];
        std::snprintf(achTime, sizeof(achTime), "%d:%02d", int(tLeft) / 60, int(tLeft) % 60);
        const glm::vec4 offTitle(120, 96, 40, 0), offText(100, 100, 100, 0), offTime(tLeft < 10.0f ? 128 : 100, tLeft < 10.0f ? 50 : 100, tLeft < 10.0f ? 40 : 100, 0);
        const std::string strTitle = std::string("TRAINING: ") + page.szTechnique;
        DrawLine(*pfont, picons, strTitle, x0 + 8.0f, y0 + 6.0f, 15.0f, offTitle);
        DrawLine(*pfont, picons, achTime, x0 + w - 8.0f - DxLine(*pfont, picons, achTime, 15.0f), y0 + 6.0f, 15.0f, offTime);
        DrawLine(*pfont, picons, def.szGoal, x0 + 8.0f, y0 + 26.0f, 12.0f, offText);
        const float u = std::clamp(s_train.progress / def.goal, 0.0f, 1.0f);
        g_hud.FillScreenRect(x0 + 8.0f, y0 + 48.0f, x0 + w - 8.0f, y0 + 60.0f, glm::vec4(30, 30, 30, 110), z - 1e-4f);
        g_hud.FillScreenRect(x0 + 8.0f, y0 + 48.0f, x0 + 8.0f + (w - 16.0f) * u, y0 + 60.0f, glm::vec4(118, 84, 24, 128), z - 5e-5f);
        char achProg[32];
        std::snprintf(achProg, sizeof(achProg), "%.0f / %.0f%s", std::min(s_train.progress, def.goal), def.goal, def.szUnit);
        DrawLine(*pfont, picons, achProg, x0 + w * 0.5f - DxLine(*pfont, picons, achProg, 10.0f) * 0.5f, y0 + 48.5f, 10.0f, offText);
    }
    if (fMsg)
    {
        // a band at the top of the picture, fading out over its last second
        const float u = std::clamp(s_tMsgEnd - g_clock.tReal, 0.0f, 1.0f);
        const float a = 128.0f * u;
        g_hud.FillScreenRect(60.0f, 26.0f, 580.0f, 76.0f, glm::vec4(0, 0, 0, 90.0f * u), z - 1e-4f);
        const glm::vec4 off1(110 * u, 90 * u, 40 * u, 0), off2(90 * u, 90 * u, 90 * u, 0);
        (void)a;
        if (s_szMsgIcon) DrawTechniqueIcon(s_szMsgIcon, 92.0f, 51.0f, 44.0f, 128.0f, 128.0f * u, z);
        const float xText = s_szMsgIcon ? 344.0f : 320.0f;   // (the text's centre, right of the icon)
        DrawLine(*pfont, picons, s_strMsg1, xText - DxLine(*pfont, picons, s_strMsg1, 17.0f) * 0.5f, 32.0f, 17.0f, off1);
        DrawLine(*pfont, picons, s_strMsg2, xText - DxLine(*pfont, picons, s_strMsg2, 14.0f) * 0.5f, 54.0f, 14.0f, off2);
    }
    g_hud.Flush(width, height);
}
