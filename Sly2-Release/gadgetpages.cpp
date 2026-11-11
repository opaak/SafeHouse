// PC addition, not in the original game: the gadget menu's pages after the grid (shop.h AgpeGadgetPage), drawn in the
// grid's own frame so they look as the grid does: the screen object stays up (the portrait, the panel, its lines and
// the description below, whose text is the page's: StrGadgetPageText), its gadget entries let go (shop.cpp
// SetGadgetPage). Drawn here over it: the page's title in place of "Gadget Grid", the entries' icons in rows (an owned
// or learned one bright, the selected one larger in a ring, its button as the grid shows it), and on the grid's page
// the hint that square leads on; on every page, faint arrows at the middle of its left and right edges.

#include "shop.h"
#include "clock.h"
#include "hud.h"
#include "screenrt.h"
#include "splayer.h"
#include "techicons.h"

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

namespace
{
// over the screen objects (as raccoonus.cpp and the debug menu); canvas pixels (640 x 448)
constexpr float DEPTH = 16760000.0f;
void DrawLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float x, float y, float size, glm::vec4 off)
{
    g_hud.DrawTextBox(font, picons, text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
float DxLine(const FONTBRX& font, const FONTBRX* picons, const std::string& text, float size) { return DxTextUi(font, picons, text, size / 51.2f); }

// The grid's frame, measured from it (canvas pixels): its panel's colour, the title's place (centred on x 364, over
// the line at y 69) and colour, in GS units (0x80 = 1)
const glm::vec4 kRgbaPanel(0.0f, 12.5f, 25.5f, 128.0f), kRgbaLine(25.5f, 38.0f, 51.0f, 128.0f);
const glm::vec4 kOffTitle(63.5f, 38.0f, 0.0f, 0.0f);
constexpr float kXTitle = 364.4f, kYTitle = 40.0f, kSizeTitle = 31.0f;
// the entries: kPageCols a row, from (kX0, kY0), kDx / kDy apart
constexpr float kX0 = 236.0f, kY0 = 134.0f, kDx = 104.0f, kDy = 72.8f, kIcon = 54.0f, kIconSel = 64.0f;   // (on the panel's bands: its lines at y 98, 170, 243, 316)
}  // namespace

void DrawGadgetPagesHud(int width, int height)
{
    const int page = IGadgetMenuPage();
    if (page < 0) return;
    const FONTBRX* pfont = PfontHud();
    const FONTBRX* picons = PfontHudIcons();
    if (!pfont) return;
    const float z = DEPTH / 16777216.0f;
    g_hud.Begin();
    // the hint: square leads to the next page (on the grid's page; on the others, the next one's name)
    {
        const int c = CGadgetPages();
        const int next = (page + 1) % c;
        const char* szNext = next == 0 ? "Gadget Grid" : next == 1 ? "Gadget Grid 2" : "Ancestor Techniques";
        const std::string str = std::string("&2S&. ") + szNext;
        DrawLine(*pfont, picons, str, 606.0f - DxLine(*pfont, picons, str, 13.0f), 421.0f, 13.0f, glm::vec4(64, 64, 64, 0));
    }
    // faint arrows at the middle of the panel's left and right edges: left / right lead to the pages before and after
    {
        const float u = 0.5f + 0.5f * std::sin(g_clock.tReal * 3.0f);
        const glm::vec4 rgba(110, 120, 128, 34.0f + 22.0f * u);
        constexpr float yMid = 180.0f, dy = 14.0f, dx = 9.0f, w = 3.0f;
        for (int side = 0; side < 2; ++side)
        {
            const float xTip = side == 0 ? 10.0f : 630.0f, s = side == 0 ? 1.0f : -1.0f;   // (the tip, toward the edge)
            const glm::vec2 tip(xTip, yMid), a(xTip + s * dx, yMid - dy), b(xTip + s * dx, yMid + dy);
            g_hud.FillScreenLine(a, tip, w, rgba, z);
            g_hud.FillScreenLine(tip, b, w, rgba, z);
        }
    }
    if (page != 0)
    {
        // the title in place of "Gadget Grid": the panel's colour over it, its line again, the page's name
        g_hud.FillScreenRect(250.0f, 38.0f, 480.0f, 73.5f, kRgbaPanel, z - 2e-4f);
        g_hud.FillScreenRect(250.0f, 68.8f, 480.0f, 69.8f, kRgbaLine, z - 1.5e-4f);
        const std::string strTitle = StrGadgetPageTitle();
        DrawLine(*pfont, picons, strTitle, kXTitle - 0.5f * DxLine(*pfont, picons, strTitle, kSizeTitle), kYTitle, kSizeTitle, kOffTitle);

        // the entries
        const std::vector<GADGETPAGEENT> aent = AgpeGadgetPage();
        const int isel = IselGadgetPage();
        static const char* const s_aszSlot[3] = {"&2L&.", "&2l&.", "&2r&."};
        for (int i = 0; i < int(aent.size()); ++i)
        {
            const GADGETPAGEENT& e = aent[size_t(i)];
            const float cx = kX0 + kDx * float(i % kPageCols), cy = kY0 + kDy * float(i / kPageCols);
            const bool fSel = i == isel;
            if (fSel)
            {
                // (a slow pulse round the selected one)
                const float u = 0.5f + 0.5f * std::sin(g_clock.tReal * 4.0f);
                const float r = 0.5f * kIconSel * (480.0f / 448.0f) + 5.0f;
                g_hud.DrawRing(cx, cy, r + 3.0f, r - 1.0f, 1.0f, glm::vec4(110, 110, 110, 60.0f + 40.0f * u), z - 1e-4f);
            }
            const float h = fSel ? kIconSel : kIcon;
            if (e.szIcon) DrawTechniqueIcon(e.szIcon, cx, cy, h, e.fOwned ? 128.0f : 30.0f, e.fOwned ? 128.0f : 100.0f, z);
            else g_hud.DrawRing(cx, cy, 0.5f * h, 0.5f * h - 3.0f, 1.0f, glm::vec4(40, 60, 80, 110), z);
            // its button, as the grid shows it (DrawGadgetIcons: a 32 x 32 box at the cell's top left)
            if (e.islot >= 0 && e.islot < 3)
                g_hud.DrawTextBox(*pfont, picons, s_aszSlot[e.islot], cx - 26.5f - 320.0f, cy * 1.1f + 8.0f - 246.4f, 0.66f * 51.2f, 1.0f,
                                  1.0f, 32.0f, 32.0f, 0, 0, glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), glm::vec4(1.0f),
                                  glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), DEPTH + 100.0f);
        }
    }
    g_hud.Flush(width, height);
}
