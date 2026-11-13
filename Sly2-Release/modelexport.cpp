// PC modding: an object's model as glTF binary (modelexport.h).

#include "modelexport.h"
#include "glob.h"
#include "shd.h"
#include "../platform/platform.h"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
struct GLBBUILD
{
    std::vector<uint8_t> bin;
    std::ostringstream bufferViews, accessors;
    int cbv = 0, cacc = 0;
    bool fFirstBv = true, fFirstAcc = true;

    int View(const void* pv, size_t cb, int target)
    {
        while (bin.size() % 4) bin.push_back(0);
        const size_t off = bin.size();
        bin.insert(bin.end(), static_cast<const uint8_t*>(pv), static_cast<const uint8_t*>(pv) + cb);
        bufferViews << (fFirstBv ? "" : ",") << "{\"buffer\":0,\"byteOffset\":" << off << ",\"byteLength\":" << cb;
        if (target) bufferViews << ",\"target\":" << target;
        bufferViews << "}";
        fFirstBv = false;
        return cbv++;
    }
    int Accessor(int bv, int componentType, size_t count, const char* type, bool fNormalized, const std::string& minmax)
    {
        accessors << (fFirstAcc ? "" : ",") << "{\"bufferView\":" << bv << ",\"componentType\":" << componentType << ",\"count\":" << count
                  << ",\"type\":\"" << type << "\"" << (fNormalized ? ",\"normalized\":true" : "") << minmax << "}";
        fFirstAcc = false;
        return cacc++;
    }
};

std::string StrMatrix(const glm::mat4& m)
{
    std::ostringstream os;
    os.precision(9);
    os << "[";
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) os << (c || r ? "," : "") << m[c][r];
    os << "]";
    return os.str();
}

std::string StrJson(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\') out += '\\';
        if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
    }
    return out;
}

void AppendPng(void* ctx, void* data, int size)
{
    auto* pv = static_cast<std::vector<uint8_t>*>(ctx);
    pv->insert(pv->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}
}  // namespace

bool FExportModelGlb(const MODELEXPORT& mx, const std::string& name, const std::string& path, std::string* pstrInfo, std::string* perr)
{
    if (!mx.pglobset) { *perr = "no model"; return false; }
    const GLOBSET& gs = *mx.pglobset;
    GLBBUILD b;
    std::ostringstream nodes, meshes, materials, textures, images;
    int cnode = 1, cmesh = 0, cmat = 0, ctex = 0, cglob = 0, ctri = 0;
    bool fFirstNode = true, fFirstMesh = true, fFirstMat = true, fFirstTex = true;
    std::vector<int> aichild;
    // the materials by (texture, blending), each texture once
    std::map<std::pair<const void*, const void*>, int> mpimage;
    std::map<std::tuple<const void*, const void*, int>, int> mpmat;
    std::vector<int> aiglobOut;

    // the frame of reference: the first shown rigid group's matrix (the others relative to it)
    glm::mat4 matRef = mx.matRef;
    for (size_t i = 0; i < gs.aglob.size() && !mx.fRef; ++i)
        if (i < mx.afVisible.size() && mx.afVisible[i] && i < mx.amatGlob.size() && (i >= mx.aamatBone.size() || mx.aamatBone[i].empty()))
        {
            matRef = mx.amatGlob[i];
            break;
        }
    const glm::mat4 matRefInv = glm::inverse(matRef);

    // the object's groups, then the other objects' parts (named apart: the import leaves them out)
    std::vector<MODELPART> asrc;
    asrc.push_back(MODELPART{mx.pglobset, mx.amatGlob, mx.aamatBone, mx.afVisible, mx.oid});
    for (const MODELPART& p : mx.apart)
        if (p.pglobset) asrc.push_back(p);
    int cpart = 0;
    for (size_t isrc = 0; isrc < asrc.size(); ++isrc)
    {
    const MODELPART& src = asrc[isrc];
    const bool fPart = isrc > 0;
    bool fPartWritten = false;
    // ("part<oid>_g<i>": not the import's "glob_<i>")
    auto nameOf = [&](size_t i) { return fPart ? "part" + std::to_string(src.oid) + "_g" + std::to_string(i) : "glob_" + std::to_string(i); };
    for (size_t i = 0; i < src.pglobset->aglob.size(); ++i)
    {
        if (i >= src.afVisible.size() || !src.afVisible[i] || i >= src.amatGlob.size()) continue;
        const GLOB& glob = src.pglobset->aglob[i];
        const GLOB* pgeom = &glob;
        if (glob.avtx.empty() && glob.iglobTemplate >= 0 && size_t(glob.iglobTemplate) < src.pglobset->aglob.size()) pgeom = &src.pglobset->aglob[size_t(glob.iglobTemplate)];
        const GLOB& geom = *pgeom;
        if (geom.meshk != MESHK_Strips || geom.avtx.empty() || geom.aindex.empty()) continue;
        const bool fPosed = i < src.aamatBone.size() && !src.aamatBone[i].empty();
        const std::vector<glm::mat4>* pabone = fPosed ? &src.aamatBone[i] : nullptr;

        // the vertices (posed: in the world; else the group's own space)
        const size_t cv = geom.avtx.size();
        std::vector<glm::vec3> apos(cv), anrm(cv);
        std::vector<glm::vec2> auv(cv);
        std::vector<uint8_t> argba(cv * 4);
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (size_t k = 0; k < cv; ++k)
        {
            const VTXGLOB& v = geom.avtx[k];
            glm::vec3 p = v.pos, n = v.normal;
            if (pabone)
            {
                glm::vec4 ps(0.0f), ns(0.0f);
                float wSum = 0.0f;
                for (int j = 0; j < 4; ++j)
                {
                    const float w = float(v.aw[j]);
                    if (w <= 0.0f) continue;
                    const glm::mat4& m = (*pabone)[std::min<size_t>(v.abone[j], pabone->size() - 1)];
                    ps += w * (m * glm::vec4(v.pos, 1.0f));
                    ns += w * (m * glm::vec4(v.normal, 0.0f));
                    wSum += w;
                }
                if (wSum > 0.0f) p = glm::vec3(ps) / wSum, n = glm::vec3(ns);
                else p = glm::vec3(src.amatGlob[i] * glm::vec4(v.pos, 1.0f)), n = glm::mat3(src.amatGlob[i]) * v.normal;
            }
            const float l = glm::length(n);
            n = l > 1e-6f ? n / l : glm::vec3(0.0f, 0.0f, 1.0f);
            apos[k] = p;
            anrm[k] = n;
            auv[k] = glm::vec2(v.uv.x, 1.0f - v.uv.y);   // (glTF: v down from the image's top; the game's rows are bottom-up)
            for (int j = 0; j < 4; ++j) argba[k * 4 + j] = uint8_t(std::min(255u, ((v.rgbaA >> (8 * j)) & 0xFF) * 2u));   // (PS2 0..128)
            lo = glm::min(lo, p), hi = glm::max(hi, p);
        }
        char achMinMax[160];
        std::snprintf(achMinMax, sizeof achMinMax, ",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        const int accPos = b.Accessor(b.View(apos.data(), apos.size() * 12, 34962), 5126, cv, "VEC3", false, achMinMax);
        const int accNrm = b.Accessor(b.View(anrm.data(), anrm.size() * 12, 34962), 5126, cv, "VEC3", false, "");
        const int accUv = b.Accessor(b.View(auv.data(), auv.size() * 8, 34962), 5126, cv, "VEC2", false, "");
        const int accCol = b.Accessor(b.View(argba.data(), argba.size(), 34962), 5121, cv, "VEC4", true, "");

        std::ostringstream prims;
        bool fFirstPrim = true;
        for (const SUBGLOB& sub : geom.asubglob)
        {
            if (!sub.indexCount || size_t(sub.firstIndex) + sub.indexCount > geom.aindex.size()) continue;
            if (sub.pshd && (sub.pshd->shdk == 2 || sub.pshd->shdk == 3)) continue;   // (the PS2's shadow shapes, the lights' volumes)
            std::vector<uint32_t> aidx(geom.aindex.begin() + sub.firstIndex, geom.aindex.begin() + sub.firstIndex + sub.indexCount);
            bool fOk = true;
            for (uint32_t ix : aidx) fOk = fOk && ix < cv;
            if (!fOk) continue;
            const int accIdx = b.Accessor(b.View(aidx.data(), aidx.size() * 4, 34963), 5125, aidx.size(), "SCALAR", false, "");
            ctri += int(aidx.size() / 3);
            // the material: the first textured pass's texture, and its blending
            const BMP* pbmp = nullptr;
            const CLUT* pclut = nullptr;
            int blend = 0;   // 0 opaque, 1 blended, 2 added (a glow)
            if (sub.pshd && !sub.pshd->apass.empty())
            {
                const SHDPASS& ps0 = sub.pshd->apass[0];
                if (ps0.fBlend)
                    blend = ((ps0.alpha >> 2) & 3) == 2 && ((ps0.alpha >> 6) & 3) == 1 ? 2 : (ps0.alpha & 0xFF) == 0x44 || ((ps0.alpha >> 4) & 3) == 2 ? 1 : 0;
                for (const SHDPASS& ps : sub.pshd->apass)
                    if (ps.fTextured)
                    {
                        const TEXSEL sel = TexselFromShd(sub.pshd, sub.psaa ? sub.psaa->iframe : 0, ps.itexPass);
                        pbmp = sel.pbmp;
                        pclut = sel.pclut;
                        break;
                    }
            }
            int imat = -1;
            // (a material per shader: the import gives its meshes that shader back; the images shared by texture)
            const auto key = std::make_tuple(static_cast<const void*>(sub.pshd), static_cast<const void*>(sub.psaa), blend);
            if (const auto it = mpmat.find(key); it != mpmat.end()) imat = it->second;
            else
            {
                int itex = -1;
                std::string nameTex;
                if (pbmp)
                {
                    const auto keyImg = std::make_pair(static_cast<const void*>(pbmp), static_cast<const void*>(pclut));
                    if (const auto iti = mpimage.find(keyImg); iti != mpimage.end()) itex = iti->second;
                    else
                    {
                        std::vector<uint8_t> rgba;
                        std::string err;
                        if (FDecodeTexture(pbmp, pclut, rgba, err))
                        {
                            const int dx = pbmp->dx, dy = pbmp->dy;
                            std::vector<uint8_t> ab(rgba.size());   // (as the texture export: rows top-down, alpha doubled)
                            for (int y = 0; y < dy; ++y)
                                for (int x = 0; x < dx; ++x)
                                    for (int k = 0; k < 4; ++k)
                                    {
                                        const uint8_t v = rgba[(size_t(y) * dx + x) * 4 + k];
                                        ab[(size_t(dy - 1 - y) * dx + x) * 4 + k] = k < 3 ? v : uint8_t(std::min(255, v * 2));
                                    }
                            std::vector<uint8_t> png;
                            if (stbi_write_png_to_func(AppendPng, &png, dx, dy, 4, ab.data(), dx * 4) && !png.empty())
                            {
                                const int bv = b.View(png.data(), png.size(), 0);
                                images << (fFirstTex ? "" : ",") << "{\"bufferView\":" << bv << ",\"mimeType\":\"image/png\",\"name\":\"tex" << ctex << "\"}";
                                textures << (fFirstTex ? "" : ",") << "{\"source\":" << ctex << ",\"sampler\":0}";
                                fFirstTex = false;
                                itex = ctex++;
                            }
                        }
                        mpimage[keyImg] = itex;
                    }
                }
                // (named by the part it came from: the import gives a mesh with this material that part's shader)
                materials << (fFirstMat ? "" : ",") << "{\"name\":\"sly2_" << (fPart ? "o" + std::to_string(src.oid) + "_" : std::string()) << "g" << i << "_s" << (&sub - geom.asubglob.data()) << (blend == 2 ? "_glow" : "")
                          << "\",\"doubleSided\":true,"
                          << "\"alphaMode\":\"" << (blend ? "BLEND" : pbmp ? "MASK" : "OPAQUE") << "\"" << (blend || !pbmp ? "" : ",\"alphaCutoff\":0.25")
                          << ",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
                if (itex >= 0) materials << ",\"baseColorTexture\":{\"index\":" << itex << "}";
                materials << "},\"extras\":{\"sly2_blend\":" << blend << (fPart ? ",\"sly2_part_oid\":" + std::to_string(src.oid) + ",\"sly2_part_glob\":" : ",\"sly2_glob\":") << i
                          << ",\"sly2_sub\":" << (&sub - geom.asubglob.data()) << "}}";
                fFirstMat = false;
                imat = cmat++;
                mpmat[key] = imat;
                (void)nameTex;
            }
            prims << (fFirstPrim ? "" : ",") << "{\"attributes\":{\"POSITION\":" << accPos << ",\"NORMAL\":" << accNrm << ",\"TEXCOORD_0\":" << accUv
                  << ",\"COLOR_0\":" << accCol << "},\"indices\":" << accIdx << ",\"material\":" << imat << "}";
            fFirstPrim = false;
        }
        if (fFirstPrim) continue;   // (no parts)
        meshes << (fFirstMesh ? "" : ",") << "{\"name\":\"" << nameOf(i) << "\",\"primitives\":[" << prims.str() << "]}";
        fFirstMesh = false;
        // its node: relative to the reference (posed: its vertices are in the world)
        const glm::mat4 matNode = fPosed ? matRefInv : matRefInv * src.amatGlob[i];
        nodes << (fFirstNode ? "" : ",") << "{\"name\":\"" << nameOf(i) << "\",\"mesh\":" << cmesh << ",\"matrix\":" << StrMatrix(matNode)
              << ",\"extras\":{" << (fPart ? "\"sly2_part_oid\":" + std::to_string(src.oid) + ",\"sly2_part_glob\":" : std::string("\"sly2_glob\":")) << i
              << ",\"sly2_posed\":" << (fPosed ? "true" : "false") << ",\"sly2_matrix\":" << StrMatrix(matNode) << "}}";
        if (!fPart) aiglobOut.push_back(int(i));
        fPartWritten = true;
        fFirstNode = false;
        aichild.push_back(cnode++);
        ++cmesh;
        ++cglob;
    }
    if (fPart && fPartWritten) ++cpart;
    }
    if (aichild.empty()) { *perr = "no groups with triangles to write"; return false; }
    if (aiglobOut.empty()) aiglobOut.push_back(-1);   // (only other objects' parts)

    // the root: the game's Z-up centimetres as glTF's Y-up metres
    const float s = 0.01f;
    glm::mat4 matRoot(0.0f);
    matRoot[0] = glm::vec4(s, 0.0f, 0.0f, 0.0f);
    matRoot[1] = glm::vec4(0.0f, 0.0f, -s, 0.0f);
    matRoot[2] = glm::vec4(0.0f, s, 0.0f, 0.0f);
    matRoot[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::ostringstream json;
    json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"SafeHouse Sly 2 model export\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],";
    json << "\"nodes\":[{\"name\":\"" << StrJson(name) << "\",\"matrix\":" << StrMatrix(matRoot) << ",\"children\":[";
    for (size_t k = 0; k < aichild.size(); ++k) json << (k ? "," : "") << aichild[k];
    json << "],\"extras\":{\"sly2_oid\":" << mx.oid << ",\"sly2_world\":" << mx.iwrec << ",\"sly2_pick_glob\":"
         << (mx.iglobPick >= 0 ? mx.iglobPick : aiglobOut.front()) << ",\"sly2_globs\":[";
    for (size_t k = 0; k < aiglobOut.size(); ++k) json << (k ? "," : "") << aiglobOut[k];
    json << "],\"sly2_units_per_metre\":100,\"sly2_root\":" << StrMatrix(matRoot) << ",\"sly2_reference\":" << StrMatrix(matRef) << "}}"
         << (fFirstNode ? "" : ",") << nodes.str() << "],";
    json << "\"meshes\":[" << meshes.str() << "],\"materials\":[" << materials.str() << "]";
    if (!fFirstTex)
        json << ",\"textures\":[" << textures.str() << "],\"images\":[" << images.str()
             << "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}]";
    while (b.bin.size() % 4) b.bin.push_back(0);
    json << ",\"buffers\":[{\"byteLength\":" << b.bin.size() << "}],\"bufferViews\":[" << b.bufferViews.str() << "],\"accessors\":["
         << b.accessors.str() << "]}";
    std::string strJson = json.str();
    while (strJson.size() % 4) strJson += ' ';

    std::error_code ec;
    fs::create_directories(cane::path(path).parent_path(), ec);
    std::ofstream out(cane::path(path), std::ios::binary);
    if (!out) { *perr = "cannot write " + path; return false; }
    auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const uint32_t cbTotal = 12 + 8 + uint32_t(strJson.size()) + 8 + uint32_t(b.bin.size());
    u32(0x46546C67);   // "glTF"
    u32(2);
    u32(cbTotal);
    u32(uint32_t(strJson.size()));
    u32(0x4E4F534A);   // "JSON"
    out.write(strJson.data(), std::streamsize(strJson.size()));
    u32(uint32_t(b.bin.size()));
    u32(0x004E4942);   // "BIN"
    out.write(reinterpret_cast<const char*>(b.bin.data()), std::streamsize(b.bin.size()));
    if (!out) { *perr = "write failed: " + path; return false; }
    char ach[160];
    if (cpart) std::snprintf(ach, sizeof ach, "%d groups (%d other objects), %d triangles, %d textures", cglob, cpart, ctri, ctex);
    else std::snprintf(ach, sizeof ach, "%d groups, %d triangles, %d textures", cglob, ctri, ctex);
    *pstrInfo = ach;
    return true;
}
