// PC modding: model mods (modelimport.h).

#include "modelimport.h"
#include "disc.h"
#include "glob.h"
#include "shd.h"
#include "texmod.h"
#include "transition.h"
#include "../platform/platform.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>
#include <stb/stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
// a material's picture as the game's texels (rows bottom-up, alpha in the PS2's 0..0x80) and their texture-mod name
using IMAGEIN = GLTFIMAGE;
// one mesh's triangles for a group: positions and normals in the group's own space, the UVs as the game's, a material
struct PRIMIN
{
    std::vector<glm::vec3> apos, anrm;
    std::vector<glm::vec2> auv;
    std::vector<uint32_t> aidx;
    int iglobSrc = -1, isubSrc = -1;   // the material's part (-1: the group's first)
    std::shared_ptr<IMAGEIN> pimg;      // its picture (a texture, or a plain colour), null: none
};
// The textures and shaders made for the mods' own pictures: kept for the world (the renderer's caches key on them;
// the texture cache is reset with each world load), freed when the world changes.
std::vector<std::unique_ptr<BMP>> s_apbmpMade;
std::vector<std::unique_ptr<SHD>> s_apshdMade;
std::map<std::pair<const BMP*, const CLUT*>, std::string> s_mpnameTex;   // (the world's textures' mod names, found once)
struct MODELMOD
{
    std::string path;
    int iwrec = -1, oid = -1;
    std::set<int> aiglobExported;                   // the groups the file replaces
    std::map<int, std::vector<PRIMIN>> mpiglobprim;  // per group its new triangles
};

std::vector<MODELMOD> s_amod;
int s_iwrecRead = -2;
std::string s_strSigRead;   // the folder's files (names, sizes, times) when read

glm::mat4 MatOfJson(const json& j)
{
    glm::mat4 m(1.0f);
    for (int k = 0; k < 16 && k < int(j.size()); ++k) glm::value_ptr(m)[k] = j[size_t(k)].get<float>();
    return m;
}

glm::mat4 MatOfNode(const json& n)
{
    if (n.contains("matrix")) return MatOfJson(n["matrix"]);
    glm::mat4 m(1.0f);
    if (n.contains("translation")) { const auto& t = n["translation"]; m = glm::translate(m, glm::vec3(t[0].get<float>(), t[1].get<float>(), t[2].get<float>())); }
    if (n.contains("rotation"))
    {
        const auto& r = n["rotation"];
        m *= glm::mat4_cast(glm::quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
    }
    if (n.contains("scale")) { const auto& s = n["scale"]; m = glm::scale(m, glm::vec3(s[0].get<float>(), s[1].get<float>(), s[2].get<float>())); }
    return m;
}

int IntOfName(const std::string& name, const std::regex& re)
{
    std::smatch m;
    return std::regex_search(name, m, re) ? std::atoi(m[1].str().c_str()) : -1;
}

// An accessor's elements as floats (cComp per element) or indices
bool FReadAccessor(const json& j, const std::vector<std::vector<uint8_t>>& abuf, int iacc, int cComp, std::vector<float>* paf, std::vector<uint32_t>* pai)
{
    if (iacc < 0 || iacc >= int(j["accessors"].size())) return false;
    const json& a = j["accessors"][size_t(iacc)];
    if (!a.contains("bufferView") || a.contains("sparse")) return false;
    const json& bv = j["bufferViews"][a["bufferView"].get<size_t>()];
    const size_t ibuf = bv.value("buffer", 0);
    if (ibuf >= abuf.size()) return false;
    const int ct = a["componentType"].get<int>();
    const size_t cbComp = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1;
    const size_t count = a["count"].get<size_t>();
    const size_t stride = bv.value("byteStride", size_t(cbComp * cComp));
    const size_t off = bv.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
    const std::vector<uint8_t>& buf = abuf[ibuf];
    if (count && off + stride * (count - 1) + cbComp * cComp > buf.size()) return false;
    const bool fNorm = a.value("normalized", false);
    for (size_t e = 0; e < count; ++e)
        for (int c = 0; c < cComp; ++c)
        {
            const uint8_t* pb = buf.data() + off + stride * e + cbComp * size_t(c);
            double v = 0;
            switch (ct)
            {
            case 5126: { float f; std::memcpy(&f, pb, 4); v = f; break; }
            case 5125: { uint32_t u; std::memcpy(&u, pb, 4); v = u; break; }
            case 5123: { uint16_t u; std::memcpy(&u, pb, 2); v = fNorm ? u / 65535.0 : u; break; }
            case 5122: { int16_t u; std::memcpy(&u, pb, 2); v = fNorm ? std::max(u / 32767.0, -1.0) : u; break; }
            case 5121: v = fNorm ? *pb / 255.0 : *pb; break;
            case 5120: { const int8_t i8 = int8_t(*pb); v = fNorm ? std::max(i8 / 127.0, -1.0) : i8; break; }
            default: return false;
            }
            if (paf) paf->push_back(float(v));
            if (pai) pai->push_back(uint32_t(v));
        }
    return true;
}

// A glTF file's JSON and buffers (GLB, or .gltf with its buffers in files beside it)
bool FLoadGltf(const fs::path& path, json& j, std::vector<std::vector<uint8_t>>& abuf, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try
    {
        if (ab.size() >= 20 && std::memcmp(ab.data(), "glTF", 4) == 0)
        {
            uint32_t cbJson = 0;
            std::memcpy(&cbJson, ab.data() + 12, 4);
            if (20 + size_t(cbJson) > ab.size()) { err = "a short GLB"; return false; }
            j = json::parse(ab.begin() + 20, ab.begin() + 20 + cbJson);
            const size_t offBin = 20 + size_t(cbJson);
            if (offBin + 8 <= ab.size())
            {
                uint32_t cbBin = 0;
                std::memcpy(&cbBin, ab.data() + offBin, 4);
                abuf.emplace_back(ab.begin() + std::ptrdiff_t(offBin + 8), ab.begin() + std::ptrdiff_t(std::min(ab.size(), offBin + 8 + cbBin)));
            }
        }
        else
        {
            j = json::parse(ab.begin(), ab.end());
            for (const json& b : j.value("buffers", json::array()))
            {
                const std::string uri = b.value("uri", "");
                if (uri.rfind("data:", 0) == 0) { err = "data URIs are not read: save as .glb or with a separate .bin"; return false; }
                std::ifstream inb(path.parent_path() / cane::path(uri), std::ios::binary);
                abuf.emplace_back((std::istreambuf_iterator<char>(inb)), std::istreambuf_iterator<char>());
            }
        }
    }
    catch (const std::exception& e) { err = std::string("not glTF: ") + e.what(); return false; }
    return true;
}

// A material's picture (once each, in mpimgMat): the base colour texture's image, else a plain base colour
std::shared_ptr<IMAGEIN> PimgOfMaterial(const json& j, const std::vector<std::vector<uint8_t>>& abuf, const fs::path& path, size_t imat,
                                        std::map<size_t, std::shared_ptr<IMAGEIN>>& mpimgMat)
{
        if (const auto it = mpimgMat.find(imat); it != mpimgMat.end()) return it->second;
        std::shared_ptr<IMAGEIN> pimg;
        const json& mat = j["materials"][imat];
        const json pbr = mat.value("pbrMetallicRoughness", json::object());
        if (pbr.contains("baseColorTexture") && j.contains("textures"))
        {
            const size_t itex = pbr["baseColorTexture"].value("index", size_t(0));
            if (itex < j["textures"].size() && j["textures"][itex].contains("source") && j.contains("images"))
            {
                const json& img = j["images"][j["textures"][itex]["source"].get<size_t>()];
                std::vector<uint8_t> abFile;
                if (img.contains("bufferView"))
                {
                    const json& bv = j["bufferViews"][img["bufferView"].get<size_t>()];
                    const size_t ibuf = bv.value("buffer", size_t(0)), off = bv.value("byteOffset", size_t(0)), cb = bv["byteLength"].get<size_t>();
                    if (ibuf < abuf.size() && off + cb <= abuf[ibuf].size()) abFile.assign(abuf[ibuf].begin() + std::ptrdiff_t(off), abuf[ibuf].begin() + std::ptrdiff_t(off + cb));
                }
                else if (img.contains("uri") && img["uri"].get<std::string>().rfind("data:", 0) != 0)
                {
                    std::ifstream ini(path.parent_path() / cane::path(img["uri"].get<std::string>()), std::ios::binary);
                    abFile.assign((std::istreambuf_iterator<char>(ini)), std::istreambuf_iterator<char>());
                }
                int dx = 0, dy = 0, cn = 0;
                if (stbi_uc* p = abFile.empty() ? nullptr : stbi_load_from_memory(abFile.data(), int(abFile.size()), &dx, &dy, &cn, 4))
                {
                    // (the export's PNG back to the game's texels: rows bottom-up, alpha halved)
                    auto pab = std::make_shared<std::vector<uint8_t>>(size_t(dx) * dy * 4);
                    for (int y = 0; y < dy; ++y)
                        for (int x = 0; x < dx; ++x)
                            for (int c = 0; c < 4; ++c)
                            {
                                const uint8_t v = p[(size_t(dy - 1 - y) * dx + x) * 4 + c];
                                (*pab)[(size_t(y) * dx + x) * 4 + c] = c < 3 ? v : uint8_t(v == 255 ? 128 : (v + 1) / 2);
                            }
                    stbi_image_free(p);
                    pimg = std::make_shared<IMAGEIN>();
                    pimg->dx = dx, pimg->dy = dy, pimg->pab = pab;
                    pimg->name = SzTextureModName(*pab, dx, dy);
                    if (j["textures"][itex].contains("sampler") && j.contains("samplers"))
                    {
                        const json& smp = j["samplers"][j["textures"][itex]["sampler"].get<size_t>()];
                        pimg->fClampS = smp.value("wrapS", 10497) == 33071;
                        pimg->fClampT = smp.value("wrapT", 10497) == 33071;
                    }
                }
            }
        }
        else if (pbr.contains("baseColorFactor"))
        {
            // a plain colour (a Blender material without a picture): one texel (glTF's linear colour to sRGB-ish)
            const json& f = pbr["baseColorFactor"];
            auto pab = std::make_shared<std::vector<uint8_t>>(4);
            for (int c = 0; c < 3; ++c) (*pab)[size_t(c)] = uint8_t(std::clamp(std::pow(f[size_t(c)].get<double>(), 1.0 / 2.2), 0.0, 1.0) * 255.0 + 0.5);
            (*pab)[3] = uint8_t(std::clamp(f[3].get<double>(), 0.0, 1.0) * 128.0 + 0.5);
            pimg = std::make_shared<IMAGEIN>();
            pimg->dx = pimg->dy = 1, pimg->pab = pab;
            pimg->name = SzTextureModName(*pab, 1, 1);
            pimg->fPlain = true;
        }
        mpimgMat[imat] = pimg;
        return pimg;
}

bool FReadModelMod(const fs::path& path, MODELMOD& mod, std::string& err)
{
    json j;
    std::vector<std::vector<uint8_t>> abuf;
    if (!FLoadGltf(path, j, abuf, err)) return false;
    if (!j.contains("nodes") || !j.contains("meshes")) { err = "no nodes or meshes"; return false; }
    const json& anode = j["nodes"];

    // the parents, and the root: the node with the game's notes, else the parent of the glob_ nodes
    std::vector<int> aiparent(anode.size(), -1);
    for (size_t k = 0; k < anode.size(); ++k)
        for (const json& c : anode[k].value("children", json::array()))
            if (c.get<size_t>() < anode.size()) aiparent[c.get<size_t>()] = int(k);
    // the nodes in the scene (a node no scene reaches is not drawn: not read)
    std::vector<bool> afInScene(anode.size(), false);
    {
        const json& ascene = j.value("scenes", json::array());
        const size_t iscene = j.value("scene", size_t(0));
        std::vector<size_t> astack;
        if (iscene < ascene.size())
            for (const json& r : ascene[iscene].value("nodes", json::array())) astack.push_back(r.get<size_t>());
        else for (size_t k = 0; k < anode.size(); ++k) if (aiparent[k] < 0) astack.push_back(k);
        while (!astack.empty())
        {
            const size_t k = astack.back();
            astack.pop_back();
            if (k >= anode.size() || afInScene[k]) continue;
            afInScene[k] = true;
            for (const json& c : anode[k].value("children", json::array())) astack.push_back(c.get<size_t>());
        }
    }
    static const std::regex s_reGlob("glob_(\\d+)"), s_reMat("sly2_g(\\d+)_s(\\d+)");
    static const std::regex s_reFile("^w(-?\\d+)_.*_oid(-?\\d+)"), s_reFileGlob("_oid-?\\d+_g(\\d+)");
    int iroot = -1;
    for (size_t k = 0; k < anode.size() && iroot < 0; ++k)
        if (anode[k].contains("extras") && anode[k]["extras"].contains("sly2_oid")) iroot = int(k);
    for (size_t k = 0; k < anode.size() && iroot < 0; ++k)
        if (IntOfName(anode[k].value("name", ""), s_reGlob) >= 0 && aiparent[k] >= 0) iroot = aiparent[k];
    const json extrasRoot = iroot >= 0 && anode[size_t(iroot)].contains("extras") ? anode[size_t(iroot)]["extras"] : json::object();

    // the world and the object: the notes, else the file name
    std::smatch m;
    const std::string nameFile = cane::utf8(path.filename());
    const bool fName = std::regex_search(nameFile, m, s_reFile);
    mod.iwrec = extrasRoot.contains("sly2_world") ? extrasRoot["sly2_world"].get<int>() : fName ? std::atoi(m[1].str().c_str()) : -1;
    mod.oid = extrasRoot.contains("sly2_oid") ? extrasRoot["sly2_oid"].get<int>() : fName ? std::atoi(m[2].str().c_str()) : -1;
    if (mod.iwrec < 0 || mod.oid < 0) { err = "its world and object not known (keep the exported file name: w<world>_..._oid<oid>...)"; return false; }
    int iglobHome = extrasRoot.contains("sly2_pick_glob") ? extrasRoot["sly2_pick_glob"].get<int>() : IntOfName(nameFile, s_reFileGlob);
    if (extrasRoot.contains("sly2_globs"))
        for (const json& g : extrasRoot["sly2_globs"]) mod.aiglobExported.insert(g.get<int>());

    // world transforms
    std::vector<glm::mat4> amatWorld(anode.size());
    std::vector<bool> afDone(anode.size(), false);
    std::function<glm::mat4(int)> world = [&](int k) -> glm::mat4 {
        if (afDone[size_t(k)]) return amatWorld[size_t(k)];
        afDone[size_t(k)] = true;   // (a cycle: stop)
        amatWorld[size_t(k)] = (aiparent[size_t(k)] >= 0 ? world(aiparent[size_t(k)]) : glm::mat4(1.0f)) * MatOfNode(anode[size_t(k)]);
        return amatWorld[size_t(k)];
    };
    // the file's root space: the root's world inverted; no root: the export's (Y-up metres) assumed
    glm::mat4 matRootInv;
    if (iroot >= 0) matRootInv = glm::inverse(world(iroot));
    else
    {
        glm::mat4 r(0.0f);
        r[0] = glm::vec4(0.01f, 0, 0, 0), r[1] = glm::vec4(0, 0, -0.01f, 0), r[2] = glm::vec4(0, 0.01f, 0, 0), r[3] = glm::vec4(0, 0, 0, 1);
        matRootInv = glm::inverse(r);
    }

    // each mesh node: its group (its notes or name, a parent's, else the home group) and its triangles in that
    // group's own space (the node's place at the export, sly2_matrix, undone; without it the node as it is now)
    std::map<size_t, std::shared_ptr<IMAGEIN>> mpimgMat;
    auto imageOfMaterial = [&](size_t imat) { return PimgOfMaterial(j, abuf, path, imat, mpimgMat); };

    bool fNotes = false;
    for (size_t k = 0; k < anode.size(); ++k)
    {
        const json& n = anode[k];
        if (!n.contains("mesh") || !afInScene[k]) continue;
        int iglob = -1;
        glm::mat4 mat0(0.0f);
        bool fMat0 = false;
        for (int p = int(k); p >= 0 && iglob < 0; p = aiparent[size_t(p)])
        {
            const json& np = anode[size_t(p)];
            if (np.contains("extras") && np["extras"].contains("sly2_glob")) iglob = np["extras"]["sly2_glob"].get<int>();
            else iglob = IntOfName(np.value("name", ""), s_reGlob);
            if (iglob >= 0 && np.contains("extras") && np["extras"].contains("sly2_matrix") && p == int(k))
                mat0 = MatOfJson(np["extras"]["sly2_matrix"]), fMat0 = true, fNotes = true;
            if (p == iroot) break;
        }
        const bool fHome = iglob < 0;
        if (fHome) iglob = iglobHome;
        if (iglob < 0) continue;
        // the node's matrix in the root's space now; the group's own space is the export's node space
        const glm::mat4 matNow = matRootInv * world(int(k));
        glm::mat4 matToGlob;
        if (fMat0) matToGlob = glm::inverse(mat0) * matNow;
        else if (!fHome) matToGlob = glm::mat4(1.0f);   // (no notes: the group's node taken as not moved)
        else
        {
            // a new mesh: where it is relative to the home group's node (its notes, else the node itself)
            glm::mat4 matHome0(1.0f);
            for (size_t h = 0; h < anode.size(); ++h)
            {
                const json& nh = anode[h];
                const int ih = nh.contains("extras") && nh["extras"].contains("sly2_glob") ? nh["extras"]["sly2_glob"].get<int>() : IntOfName(nh.value("name", ""), s_reGlob);
                if (ih != iglob) continue;
                matHome0 = nh.contains("extras") && nh["extras"].contains("sly2_matrix") ? MatOfJson(nh["extras"]["sly2_matrix"]) : matRootInv * world(int(h));
                break;
            }
            matToGlob = glm::inverse(matHome0) * matNow;
        }
        const glm::mat3 matNrm = glm::transpose(glm::inverse(glm::mat3(matToGlob)));
        const json& mesh = j["meshes"][n["mesh"].get<size_t>()];
        for (const json& prim : mesh.value("primitives", json::array()))
        {
            if (prim.value("mode", 4) != 4 || !prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
            const json& attr = prim["attributes"];
            std::vector<float> afPos, afNrm, afUv;
            std::vector<uint32_t> aidx;
            if (!FReadAccessor(j, abuf, attr["POSITION"].get<int>(), 3, &afPos, nullptr)) continue;
            const size_t cv = afPos.size() / 3;
            if (attr.contains("NORMAL")) FReadAccessor(j, abuf, attr["NORMAL"].get<int>(), 3, &afNrm, nullptr);
            if (attr.contains("TEXCOORD_0")) FReadAccessor(j, abuf, attr["TEXCOORD_0"].get<int>(), 2, &afUv, nullptr);
            if (prim.contains("indices")) FReadAccessor(j, abuf, prim["indices"].get<int>(), 1, nullptr, &aidx);
            else for (uint32_t v = 0; v < cv; ++v) aidx.push_back(v);
            if (aidx.size() % 3 || std::any_of(aidx.begin(), aidx.end(), [cv](uint32_t v) { return v >= cv; })) continue;
            PRIMIN pi;
            for (size_t v = 0; v < cv; ++v)
            {
                pi.apos.push_back(glm::vec3(matToGlob * glm::vec4(afPos[v * 3], afPos[v * 3 + 1], afPos[v * 3 + 2], 1.0f)));
                glm::vec3 nn = afNrm.size() == cv * 3 ? matNrm * glm::vec3(afNrm[v * 3], afNrm[v * 3 + 1], afNrm[v * 3 + 2]) : glm::vec3(0.0f, 0.0f, 1.0f);
                const float l = glm::length(nn);
                pi.anrm.push_back(l > 1e-6f ? nn / l : glm::vec3(0.0f, 0.0f, 1.0f));
                pi.auv.push_back(afUv.size() == cv * 2 ? glm::vec2(afUv[v * 2], 1.0f - afUv[v * 2 + 1]) : glm::vec2(0.0f));
            }
            pi.aidx = std::move(aidx);
            if (prim.contains("material"))
            {
                pi.pimg = imageOfMaterial(prim["material"].get<size_t>());
                const json& mat = j["materials"][prim["material"].get<size_t>()];
                std::smatch mm;
                const std::string nameMat = mat.value("name", "");
                if (mat.contains("extras") && mat["extras"].contains("sly2_glob"))
                    pi.iglobSrc = mat["extras"]["sly2_glob"].get<int>(), pi.isubSrc = mat["extras"].value("sly2_sub", 0);
                else if (std::regex_search(nameMat, mm, s_reMat))
                    pi.iglobSrc = std::atoi(mm[1].str().c_str()), pi.isubSrc = std::atoi(mm[2].str().c_str());
            }
            mod.mpiglobprim[iglob].push_back(std::move(pi));
        }
        if (!fHome || iglobHome >= 0) mod.aiglobExported.insert(iglob);
    }
    if (mod.mpiglobprim.empty() && mod.aiglobExported.empty()) { err = "no meshes"; return false; }
    if (!fNotes && !extrasRoot.contains("sly2_globs"))
        std::printf("model mod %s: no custom properties: the groups' objects are taken as not moved and a deleted group "
                    "stays (Blender's glTF export: Include > Custom Properties keeps them)\n", nameFile.c_str());
    return true;
}

void ReadModelMods()
{
    s_amod.clear();
    if (s_iwrecRead != g_transition.iwrecCur)
    {
        // (another world: its textures and shaders are no one's any more; the same world's stay, its groups may use them)
        s_apbmpMade.clear();
        s_apshdMade.clear();
        s_mpnameTex.clear();
    }
    s_iwrecRead = g_transition.iwrecCur;
    if (g_disc.DirMods().empty()) return;
    const fs::path dir = cane::path(g_disc.DirMods()) / "Models";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file()) continue;
        std::string ext = cane::utf8(e.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".glb" && ext != ".gltf") continue;
        MODELMOD mod;
        mod.path = cane::utf8(e.path());
        std::string err;
        if (!FReadModelMod(e.path(), mod, err)) { std::printf("model mod %s: skipped: %s\n", mod.path.c_str(), err.c_str()); continue; }
        if (mod.iwrec != g_transition.iwrecCur) continue;
        std::printf("model mod %s: world %d object %d, %zu groups\n", cane::utf8(e.path().filename()).c_str(), mod.iwrec, mod.oid, mod.aiglobExported.size());
        s_amod.push_back(std::move(mod));
    }
}

// the group's geometry (an instance's is its template's)
const GLOB* PglobGeom(const GLOBSET& gs, int i)
{
    if (i < 0 || size_t(i) >= gs.aglob.size()) return nullptr;
    const GLOB& g = gs.aglob[size_t(i)];
    if (g.avtx.empty() && g.iglobTemplate >= 0 && size_t(g.iglobTemplate) < gs.aglob.size()) return &gs.aglob[size_t(g.iglobTemplate)];
    return &g;
}

// a part's texture's mod name as shown (its first textured pass, the animation's frame): "" untextured
std::string StrTextureNameOfShd(const SHD* pshd, const SAA* psaa)
{
    if (!pshd) return std::string();
    for (const SHDPASS& ps : pshd->apass)
    {
        if (!ps.fTextured) continue;
        const TEXSEL sel = TexselFromShd(pshd, psaa ? psaa->iframe : 0, ps.itexPass);
        if (!sel.pbmp) return std::string();
        const auto key = std::make_pair(static_cast<const BMP*>(sel.pbmp), static_cast<const CLUT*>(sel.pclut));
        if (const auto it = s_mpnameTex.find(key); it != s_mpnameTex.end()) return it->second;
        std::vector<uint8_t> rgba;
        std::string err;
        std::string name = FDecodeTexture(sel.pbmp, sel.pclut, rgba, err) ? SzTextureModName(rgba, sel.pbmp->dx, sel.pbmp->dy) : std::string();
        s_mpnameTex[key] = name;
        return name;
    }
    return std::string();
}

glm::vec4 SphereOfVerts(const std::vector<VTXGLOB>& av)
{
    if (av.empty()) return glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const VTXGLOB& v : av) lo = glm::min(lo, v.pos), hi = glm::max(hi, v.pos);
    return glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f);
}
}  // namespace

void BeginModelMods()
{
    std::string sig;
    if (!g_disc.DirMods().empty())
    {
        std::error_code ec;
        const fs::path dir = cane::path(g_disc.DirMods()) / "Models";
        if (fs::is_directory(dir, ec))
            for (const auto& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file(ec))
                    sig += cane::utf8(e.path().filename()) + ":" + std::to_string(e.file_size(ec)) + ":" +
                           std::to_string(e.last_write_time(ec).time_since_epoch().count()) + ";";
    }
    if (s_iwrecRead != g_transition.iwrecCur || sig != s_strSigRead)
    {
        ReadModelMods();
        s_strSigRead = sig;
    }
}

void ApplyModelMods(GLOBSET& gs, int oid)
{
    if (gs.fModelModsTried) return;
    gs.fModelModsTried = true;
    if (s_amod.empty() || oid < 0) return;
    for (const MODELMOD& mod : s_amod)
    {
        if (mod.oid != oid) continue;
        // the groups' own source parts copied first (a material may name a group this file replaces)
        std::vector<SUBGLOB> asubSrcFirst(gs.aglob.size());
        std::map<std::pair<int, int>, SUBGLOB> mpsubSrc;
        for (int i = 0; i < int(gs.aglob.size()); ++i)
            if (const GLOB* pg = PglobGeom(gs, i))
                for (int s = 0; s < int(pg->asubglob.size()); ++s) mpsubSrc[{i, s}] = pg->asubglob[size_t(s)];
        int cRepl = 0, cGone = 0, cKept = 0, cCel = 0, cTex = 0;
        // (each picture made once, and each picture's shader once per source shader)
        std::map<const IMAGEIN*, BMP*> mpbmp;
        std::map<std::pair<const IMAGEIN*, const SHD*>, SHD*> mpshd;
        std::vector<glm::vec4> asphereOld;
        for (int iglob : mod.aiglobExported)
        {
            if (iglob < 0 || size_t(iglob) >= gs.aglob.size()) continue;
            GLOB& glob = gs.aglob[size_t(iglob)];
            if (glob.meshk != MESHK_Strips || glob.avtx.empty() || glob.pgl || glob.pwrbg || glob.cbone > 0 || !glob.abone.empty())
            {
                ++cKept;   // (an instance, skinned, warped, or already made for the GPU: left as it is)
                continue;
            }
            const std::vector<VTXGLOB> avtxOld = glob.avtx;
            const glm::vec4 sphereOld = SphereOfVerts(avtxOld);
            asphereOld.push_back(sphereOld);
            const auto it = mod.mpiglobprim.find(iglob);
            std::vector<VTXGLOB> avtx;
            std::vector<uint32_t> aindex;
            std::vector<SUBGLOB> asub;
            if (it != mod.mpiglobprim.end())
                for (const PRIMIN& pi : it->second)
                {
                    SUBGLOB sub;
                    const auto its = mpsubSrc.find({pi.iglobSrc, pi.isubSrc});
                    if (its != mpsubSrc.end()) sub = its->second;
                    else if (!avtxOld.empty() && mpsubSrc.count({iglob, 0})) sub = mpsubSrc[{iglob, 0}];
                    sub.pgpu = nullptr;
                    sub.grf &= ~0x20u;   // (no colours from the index table: the vertices' own)
                    // (a part's own material as exported, untextured: Blender gives it a plain colour; the part kept)
                    const bool fOwnPlain = pi.pimg && pi.pimg->fPlain && its != mpsubSrc.end();
                    if (pi.pimg && pi.pimg->pab && !fOwnPlain && pi.pimg->name != StrTextureNameOfShd(sub.pshd, sub.psaa))
                    {
                        // a picture of its own (not the part's texture as exported): its texture (32-bit texels, as the
                        // game's own PSMCT32), and the part's shader copied with it as its only frame (no animation)
                        BMP*& pbmpMade = mpbmp[pi.pimg.get()];
                        if (!pbmpMade)
                        {
                            auto pbmp = std::make_unique<BMP>();
                            pbmp->dx = uint16_t(pi.pimg->dx), pbmp->dy = uint16_t(pi.pimg->dy);
                            pbmp->psm = 0;
                            pbmp->cb = uint32_t(pi.pimg->pab->size());
                            pbmp->pab = pi.pimg->pab;
                            pbmpMade = pbmp.get();
                            s_apbmpMade.push_back(std::move(pbmp));
                        }
                        SHD*& pshdMade = mpshd[{pi.pimg.get(), sub.pshd}];
                        if (pshdMade)
                        {
                            sub.pshd = pshdMade;
                            sub.psaa = nullptr;
                            ++cTex;
                        }
                        else
                        {
                        auto pshd = std::make_unique<SHD>(sub.pshd ? *sub.pshd : SHD{});
                        if (pshd->apass.empty()) pshd->apass.push_back(SHDPASS{});
                        bool fTextured = false;
                        for (const SHDPASS& ps : pshd->apass) fTextured = fTextured || ps.fTextured;
                        if (!fTextured) pshd->apass[0].fTextured = true, pshd->apass[0].itexPass = 0;
                        TEX tex;
                        tex.grftex = uint16_t((pi.pimg->fClampS ? 1 : 0) | (pi.pimg->fClampT ? 2 : 0));
                        tex.pshd = pshd.get();
                        tex.apbmp.push_back(pbmpMade);
                        pshd->atex.assign(1, tex);
                        pshd->cframe = 1;
                        pshd->psaa = nullptr;
                        sub.pshd = pshd.get();
                        sub.psaa = nullptr;
                        pshdMade = pshd.get();
                        s_apshdMade.push_back(std::move(pshd));
                        ++cTex;
                        }
                    }
                    sub.ivtxFirst = uint32_t(avtx.size());
                    sub.cvtx = uint32_t(pi.apos.size());
                    sub.firstIndex = uint32_t(aindex.size());
                    sub.indexCount = uint32_t(pi.aidx.size());
                    for (size_t v = 0; v < pi.apos.size(); ++v)
                    {
                        // the light and the rest from the nearest original vertex; the place, normal and UV its own
                        size_t iBest = 0;
                        float d2Best = std::numeric_limits<float>::max();
                        for (size_t o = 0; o < avtxOld.size(); ++o)
                        {
                            const glm::vec3 d = avtxOld[o].pos - pi.apos[v];
                            const float d2 = glm::dot(d, d);
                            if (d2 < d2Best) d2Best = d2, iBest = o;
                        }
                        VTXGLOB vn = avtxOld.empty() ? VTXGLOB{} : avtxOld[iBest];
                        if (avtxOld.empty()) vn.rgbaA = vn.rgbaB = vn.rgbaSrc = 0x80808080u;
                        vn.pos = pi.apos[v];
                        vn.normal = pi.anrm[v];
                        vn.uv = pi.auv[v];
                        vn.fAdc = 0;
                        avtx.push_back(vn);
                    }
                    for (uint32_t ix : pi.aidx) aindex.push_back(sub.ivtxFirst + ix);
                    asub.push_back(sub);
                }
            glob.avtx = std::move(avtx);
            glob.aindex = std::move(aindex);
            glob.asubglob = std::move(asub);
            glob.anrmSmooth.clear();
            glob.pidxtab.reset();
            glob.pvtxLitKey = nullptr;
            // its spheres: both the old and the new place (the culling)
            const glm::vec4 sphereNew = SphereOfVerts(glob.avtx);
            if (sphereNew.w >= 0.0f)
            {
                const glm::vec3 lo = glm::min(glm::vec3(sphereOld) - sphereOld.w, glm::vec3(sphereNew) - sphereNew.w);
                const glm::vec3 hi = glm::max(glm::vec3(sphereOld) + sphereOld.w, glm::vec3(sphereNew) + sphereNew.w);
                const glm::vec3 c = (lo + hi) * 0.5f;
                const float r = glm::length(hi - lo) * 0.5f;
                if (glob.fDraw) glob.posCenter = c, glob.sRadius = std::max(glob.sRadius, r);
                if (glob.fSphere) glob.posSphere = c, glob.sRadiusSphere = std::max(glob.sRadiusSphere, r);
                ++cRepl;
            }
            else ++cGone;
        }
        // the PS2's cel borders (type 1) of the replaced groups' old shapes: gone with them (those within an old shape)
        for (GLOB& glob : gs.aglob)
        {
            if (glob.meshk != MESHK_CelBorder || glob.asubcel.empty()) continue;
            glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            for (const SUBCEL& sc : glob.asubcel)
                for (const glm::vec3& p : sc.apos) lo = glm::min(lo, p), hi = glm::max(hi, p);
            const glm::vec3 c = (lo + hi) * 0.5f;
            const float r = glm::length(hi - lo) * 0.5f;
            for (const glm::vec4& s : asphereOld)
                if (s.w >= 0.0f && glm::length(c - glm::vec3(s)) <= s.w * 0.25f + 1.0f && r <= s.w * 1.2f + 1.0f)
                {
                    glob.asubcel.clear();
                    ++cCel;
                    break;
                }
        }
        std::printf("model mod: object %d: %d groups replaced, %d gone, %d left as they were (skinned, instanced, warped), %d cel borders gone, "
                    "%d parts with their own pictures\n", oid, cRepl, cGone, cKept, cCel, cTex);
    }
}

bool FReadGltfPrims(const std::string& pathStr, std::vector<GLTFPRIM>& aprim, bool* pfGameExport, std::string& err)
{
    const fs::path path = cane::path(pathStr);
    json j;
    std::vector<std::vector<uint8_t>> abuf;
    if (!FLoadGltf(path, j, abuf, err)) return false;
    if (!j.contains("nodes") || !j.contains("meshes")) { err = "no nodes or meshes"; return false; }
    const json& anode = j["nodes"];
    *pfGameExport = false;
    for (const json& n : anode)
        if (n.contains("extras") && n["extras"].contains("sly2_oid")) *pfGameExport = true;
    // glTF's Y-up metres as the game's Z-up centimetres (the export's root undone)
    glm::mat4 matConv(0.0f);
    matConv[0] = glm::vec4(100.0f, 0, 0, 0), matConv[1] = glm::vec4(0, 0, 100.0f, 0), matConv[2] = glm::vec4(0, -100.0f, 0, 0), matConv[3] = glm::vec4(0, 0, 0, 1);
    std::map<size_t, std::shared_ptr<IMAGEIN>> mpimg;
    // the scene's nodes, walked from its roots with their world matrices
    std::vector<std::pair<size_t, glm::mat4>> astack;
    const json ascene = j.value("scenes", json::array());
    const size_t iscene = j.value("scene", size_t(0));
    if (iscene < ascene.size())
        for (const json& r : ascene[iscene].value("nodes", json::array())) astack.push_back({r.get<size_t>(), glm::mat4(1.0f)});
    std::set<size_t> setSeen;
    while (!astack.empty())
    {
        const auto [k, matParent] = astack.back();
        astack.pop_back();
        if (k >= anode.size() || !setSeen.insert(k).second) continue;
        const json& n = anode[k];
        const glm::mat4 matWorld = matParent * MatOfNode(n);
        for (const json& c : n.value("children", json::array())) astack.push_back({c.get<size_t>(), matWorld});
        if (!n.contains("mesh")) continue;
        const glm::mat4 mat = matConv * matWorld;
        const glm::mat3 matNrm = glm::transpose(glm::inverse(glm::mat3(mat)));
        for (const json& prim : j["meshes"][n["mesh"].get<size_t>()].value("primitives", json::array()))
        {
            if (prim.value("mode", 4) != 4 || !prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
            const json& attr = prim["attributes"];
            std::vector<float> afPos, afNrm, afUv;
            std::vector<uint32_t> aidx;
            if (!FReadAccessor(j, abuf, attr["POSITION"].get<int>(), 3, &afPos, nullptr)) continue;
            const size_t cv = afPos.size() / 3;
            if (attr.contains("NORMAL")) FReadAccessor(j, abuf, attr["NORMAL"].get<int>(), 3, &afNrm, nullptr);
            if (attr.contains("TEXCOORD_0")) FReadAccessor(j, abuf, attr["TEXCOORD_0"].get<int>(), 2, &afUv, nullptr);
            if (prim.contains("indices")) FReadAccessor(j, abuf, prim["indices"].get<int>(), 1, nullptr, &aidx);
            else for (uint32_t v = 0; v < cv; ++v) aidx.push_back(v);
            if (aidx.size() % 3 || std::any_of(aidx.begin(), aidx.end(), [cv](uint32_t v) { return v >= cv; })) continue;
            GLTFPRIM gp;
            for (size_t v = 0; v < cv; ++v)
            {
                gp.apos.push_back(glm::vec3(mat * glm::vec4(afPos[v * 3], afPos[v * 3 + 1], afPos[v * 3 + 2], 1.0f)));
                glm::vec3 nn = afNrm.size() == cv * 3 ? matNrm * glm::vec3(afNrm[v * 3], afNrm[v * 3 + 1], afNrm[v * 3 + 2]) : glm::vec3(0.0f, 0.0f, 1.0f);
                const float l = glm::length(nn);
                gp.anrm.push_back(l > 1e-6f ? nn / l : glm::vec3(0.0f, 0.0f, 1.0f));
                gp.auv.push_back(afUv.size() == cv * 2 ? glm::vec2(afUv[v * 2], 1.0f - afUv[v * 2 + 1]) : glm::vec2(0.0f));
            }
            gp.aidx = std::move(aidx);
            if (prim.contains("material"))
            {
                const size_t imat = prim["material"].get<size_t>();
                gp.pimg = PimgOfMaterial(j, abuf, path, imat, mpimg);
                const std::string mode = j["materials"][imat].value("alphaMode", "OPAQUE");
                gp.alphaMode = mode == "BLEND" ? 2 : mode == "MASK" ? 1 : 0;
            }
            aprim.push_back(std::move(gp));
        }
    }
    if (aprim.empty()) { err = "no triangles"; return false; }
    return true;
}
