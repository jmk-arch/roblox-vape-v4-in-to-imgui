// vape_assets.cpp - 원본 PNG 를 D3D11 텍스처로 로드

#include "vape_assets.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <d3d11.h>

#include <cmath>
#include <vector>

namespace vape {

namespace {

// Asset enum 순서와 1:1 대응하는 파일명
const char* kAssetFiles[(int)Asset::COUNT] = {
    "add", "alert", "allowedicon", "allowedtab", "arrowmodule", "back", "bind", "bindbkg",
    "blatanticon", "blockedicon", "blockedtab", "blur", "blurnotif", "close", "closemini",
    "colorpreview", "combaticon", "customsettings", "discord", "dots", "edit",
    "expandicon", "expandright", "expandup", "friendstab", "guisettings", "guislider",
    "guisliderrain", "guiv4", "guivape", "info", "inventoryicon", "legit", "legittab",
    "miniicon", "notification", "overlaysicon", "overlaystab", "pin", "profilesicon",
    "radaricon", "rainbow_1", "rainbow_2", "rainbow_3", "rainbow_4", "range", "rangearrow",
    "rendericon", "rendertab", "search", "targetinfoicon", "targetnpc1", "targetnpc2",
    "targetplayers1", "targetplayers2", "targetstab", "textguiicon", "textv4",
    "textvape", "utilityicon", "vape", "warning", "worldicon",
};

AssetTexture g_tex[(int)Asset::COUNT];
std::vector<ID3D11ShaderResourceView*> g_srvs;
bool g_loaded = false;

} // namespace

int LoadAssets(void* d3dDevice, const std::string& assetDir) {
    ID3D11Device* dev = (ID3D11Device*)d3dDevice;
    if (!dev) return 0;

    int ok = 0;
    for (int i = 0; i < (int)Asset::COUNT; i++) {
        std::string path = assetDir + "/" + kAssetFiles[i] + ".png";

        int w = 0, h = 0, ch = 0;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) continue;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sub = {};
        sub.pSysMem = data;
        sub.SysMemPitch = w * 4;

        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&desc, &sub, &tex)) && tex) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
            srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels = 1;

            ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(dev->CreateShaderResourceView(tex, &srvd, &srv)) && srv) {
                g_tex[i].ID = (ImTextureID)srv;
                g_tex[i].Width = w;
                g_tex[i].Height = h;
                g_tex[i].Valid = true;
                g_srvs.push_back(srv);
                ok++;
            }
            tex->Release();
        }
        stbi_image_free(data);
    }

    g_loaded = ok > 0;
    return ok;
}

void UnloadAssets() {
    for (auto* s : g_srvs) if (s) s->Release();
    g_srvs.clear();
    for (int i = 0; i < (int)Asset::COUNT; i++) g_tex[i] = AssetTexture{};
    g_loaded = false;
}

const AssetTexture& GetAsset(Asset a) { return g_tex[(int)a]; }
bool AssetsLoaded() { return g_loaded; }

bool DrawAsset(ImDrawList* dl, Asset a, ImVec2 min, ImVec2 max, ImU32 tint) {
    const AssetTexture& t = g_tex[(int)a];
    if (!t.Valid) return false;
    dl->AddImage(t.ID, min, max, ImVec2(0, 0), ImVec2(1, 1), tint);
    return true;
}

bool DrawAssetSized(ImDrawList* dl, Asset a, ImVec2 topLeft, float w, float h, ImU32 tint) {
    return DrawAsset(dl, a, topLeft, ImVec2(topLeft.x + w, topLeft.y + h), tint);
}

bool DrawAssetRotated(ImDrawList* dl, Asset a, ImVec2 center, float w, float h,
                      float rotDeg, ImU32 tint) {
    const AssetTexture& t = g_tex[(int)a];
    if (!t.Valid) return false;

    float r = rotDeg * 3.14159265f / 180.0f;
    float c = std::cos(r), s = std::sin(r);
    float hw = w * 0.5f, hh = h * 0.5f;

    auto rot = [&](float x, float y) {
        return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
    };
    ImVec2 p1 = rot(-hw, -hh), p2 = rot(hw, -hh);
    ImVec2 p3 = rot(hw, hh),   p4 = rot(-hw, hh);

    dl->AddImageQuad(t.ID, p1, p2, p3, p4,
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
    return true;
}

bool DrawAssetSliced(ImDrawList* dl, Asset a, ImVec2 min, ImVec2 max,
                     float srcX, float srcY, float srcW, float srcH,
                     float borderL, float borderT, float borderR, float borderB,
                     ImU32 tint, float destinationScale) {
    const AssetTexture& t = g_tex[(int)a];
    if (!t.Valid || t.Width <= 0 || t.Height <= 0) return false;

    // 가로/세로 3구간의 화면 좌표와 원본 픽셀 좌표
    const float xs[4] = { min.x, min.x + borderL * destinationScale,
                          max.x - borderR * destinationScale, max.x };
    const float ys[4] = { min.y, min.y + borderT * destinationScale,
                          max.y - borderB * destinationScale, max.y };
    const float us[4] = { srcX, srcX + borderL, srcX + srcW - borderR, srcX + srcW };
    const float vs[4] = { srcY, srcY + borderT, srcY + srcH - borderB, srcY + srcH };

    const float tw = (float)t.Width, th = (float)t.Height;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (r == 1 && c == 1) continue;              // 가운데는 창 본체가 덮으므로 생략
            if (xs[c + 1] <= xs[c] || ys[r + 1] <= ys[r]) continue;
            dl->AddImage(t.ID, ImVec2(xs[c], ys[r]), ImVec2(xs[c + 1], ys[r + 1]),
                         ImVec2(us[c] / tw, vs[r] / th),
                         ImVec2(us[c + 1] / tw, vs[r + 1] / th), tint);
        }
    }
    if (xs[2] > xs[1] && ys[2] > ys[1]) {
        dl->AddImage(t.ID, ImVec2(xs[1], ys[1]), ImVec2(xs[2], ys[2]),
                     ImVec2(us[1] / tw, vs[1] / th),
                     ImVec2(us[2] / tw, vs[2] / th), tint);
    }
    return true;
}

} // namespace vape
