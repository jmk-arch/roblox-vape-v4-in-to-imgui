// vape_gui.cpp - Vape V4 ClickGUI 이식 구현
//
// 주석의 "원본 NNNN" 은 newvape/guis/new.lua 의 줄 번호를 가리킨다.

#include "vape_gui.h"
#include "vape_assets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // windows.h 의 min/max 매크로가 std::min/max 를 깨뜨린다
#include <windows.h>
#include <shellapi.h>
#endif

namespace vape {

Palette g_palette;
ImFont* g_fontSmall = nullptr;
ImFont* g_fontBox = nullptr;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontItem = nullptr;
ImFont* g_fontSemiBold = nullptr;

// ===========================================================================
// color 모듈
// ===========================================================================

static ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
static ImU32  U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImU32 Light(ImU32 base, float amount) {
    ImVec4 c = V4(base);
    c.x += (1.0f - c.x) * amount;
    c.y += (1.0f - c.y) * amount;
    c.z += (1.0f - c.z) * amount;
    return U32(c);
}

ImU32 Dark(ImU32 base, float amount) {
    ImVec4 c = V4(base);
    c.x *= (1.0f - amount);
    c.y *= (1.0f - amount);
    c.z *= (1.0f - amount);
    return U32(c);
}

static ImU32 HSV(float h, float s, float v, float a = 1.0f) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(a * 255));
}

ImU32 Palette::Accent() const { return HSV(Hue, Sat, Value); }

ImU32 Palette::AccentStep(int index, float step) const {
    if (!Rainbow) return Accent();
    if (RainbowMode == 0) return Accent();
    float h = std::fmod(Hue - (float)index * step, 1.0f);
    if (h < 0.0f) h += 1.0f;
    if (RainbowMode == 2) h = std::floor(h * 8.0f) / 8.0f;
    return HSV(h, Sat, Value);
}

// ===========================================================================
// 트윈  (tween:Tween - 원본은 0.16초 Linear)
// ===========================================================================

namespace {

bool g_blurEnabled = true;

void FireOption(Option* opt, bool final = false) {
    if (!opt) return;
    if (opt->Function) opt->Function();
    if (opt->Changed) opt->Changed(*opt, final);
}

bool ParseFiniteFloat(const std::string& text, float& value) {
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text.c_str(), &end);
    while (end && *end && std::isspace((unsigned char)*end)) end++;
    if (!end || end == text.c_str() || *end || errno == ERANGE || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

struct AnimStore {
    std::unordered_map<ImGuiID, float> v;

    // 원본은 Linear 이므로 고정 속도로 목표까지 이동시킨다.
    float Step(ImGuiID id, float target, float dur) {
        float dt = ImGui::GetIO().DeltaTime;
        auto it = v.find(id);
        if (it == v.end()) { v[id] = target; return target; }
        float& cur = it->second;
        if (dur <= 0.0f) { cur = target; return cur; }
        float d = target - cur;
        float stepAmt = dt / dur;      // 0..1 범위 기준 진행량
        // 목표까지 남은 거리를 Linear 로 좁힌다.
        float move = (d > 0 ? 1.0f : -1.0f) * stepAmt;
        if (std::fabs(move) >= std::fabs(d)) cur = target;
        else cur += move;
        return cur;
    }
};
AnimStore g_anim;

// 0..1 정규화 트윈
float Tween01(ImGuiID id, bool on, float dur = -1.0f) {
    if (dur < 0.0f) dur = g_palette.Tween;
    return g_anim.Step(id, on ? 1.0f : 0.0f, dur);
}

// 임의 값 트윈 (위치, 크기 등)
float TweenVal(ImGuiID id, float target, float dur, float range) {
    if (dur <= 0.0f) return target;
    float dt = ImGui::GetIO().DeltaTime;
    auto it = g_anim.v.find(id);
    if (it == g_anim.v.end()) { g_anim.v[id] = target; return target; }
    float& cur = it->second;
    float d = target - cur;
    float move = (d > 0 ? 1.0f : -1.0f) * (range * dt / dur);
    if (std::fabs(move) >= std::fabs(d)) cur = target;
    else cur += move;
    return cur;
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = V4(a), y = V4(b);
    return U32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                      x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

// ---------------------------------------------------------------------------
// 아이콘 (ImDrawList 로 직접 그림 - PNG 대체)
// ---------------------------------------------------------------------------

// expandright.png - 4x8, 이미 오른쪽('>')을 향한다. rotation 은 도(°) 단위로
// 원본 ImageLabel.Rotation 과 같은 의미: 0='>', 90='v', 180='<', 270='^'.
// drawW/drawH 는 회전 전 스프라이트의 화면상 크기(원본 Size 그대로).
void Arrow(ImDrawList* dl, ImVec2 c, float drawW, float drawH, float rotDeg, ImU32 col) {
    if (DrawAssetRotated(dl, Asset::ExpandRight, c, drawW, drawH, rotDeg, col)) return;

    // 폴백: 오른쪽을 향한 삼각형을 같은 방식으로 회전
    float w = drawH, h = drawW;   // 뾰족한 방향 길이 = 원본의 세로(8)
    float r = rotDeg * 3.14159265f / 180.0f;
    float cs = std::cos(r), sn = std::sin(r);
    auto R = [&](ImVec2 p) {
        return ImVec2(c.x + p.x * cs - p.y * sn, c.y + p.x * sn + p.y * cs);
    };
    dl->AddTriangleFilled(R(ImVec2(-w * 0.5f, -h * 0.5f)),
                          R(ImVec2(-w * 0.5f, h * 0.5f)),
                          R(ImVec2(w * 0.5f, 0.0f)), col);
}

// close.png / closemini.png
// closemini.png 이 없으면 같은 X 글리프인 close.png 로, 그것도 없으면 선으로 그린다.
void Cross(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th = 1.5f) {
    ImVec2 tl(c.x - s * 0.5f, c.y - s * 0.5f);
    if (DrawAssetSized(dl, Asset::CloseMini, tl, s, s, col)) return;
    if (DrawAssetSized(dl, Asset::Close, tl, s, s, col)) return;
    float h = s * 0.3f;   // PNG 는 가장자리에 여백이 있으므로 글리프는 더 작다.
    dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, th);
    dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h, c.y + h), col, th);
}

// add.png
void Plus(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th = 1.6f) {
    if (DrawAssetSized(dl, Asset::Add, ImVec2(c.x - s * 0.5f, c.y - s * 0.5f),
                       s, s, col)) return;
    float h = s * 0.5f;
    dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), col, th);
    dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h), col, th);
}

// dots.png - 3x16
void Dots(ImDrawList* dl, ImVec2 topLeft, ImU32 col) {
    if (DrawAssetSized(dl, Asset::Dots, topLeft, 3.0f, 16.0f, col)) return;
    for (int i = 0; i < 3; i++)
        dl->AddCircleFilled(ImVec2(topLeft.x + 1.5f, topLeft.y + 2.0f + i * 6.0f), 1.5f, col, 6);
}

// range.png - TwoSlider 노브 9x16. 원본은 최대쪽 노브를 Rotation 180 으로 뒤집는다.
void RangeKnob(ImDrawList* dl, ImVec2 c, float w, float h, bool flip, ImU32 col) {
    if (DrawAssetRotated(dl, Asset::Range, c, w, h, flip ? 180.0f : 0.0f, col)) return;
    float hw = w * 0.5f, hh = h * 0.5f;
    if (!flip) {
        dl->AddTriangleFilled(ImVec2(c.x + hw, c.y - hh), ImVec2(c.x + hw, c.y + hh),
                              ImVec2(c.x - hw, c.y), col);
    } else {
        dl->AddTriangleFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x - hw, c.y + hh),
                              ImVec2(c.x + hw, c.y), col);
    }
}

// rainbow_1..4.png
void RainbowStrip(ImDrawList* dl, ImVec2 tl, float w, float h, float lit, ImU32 off) {
    // 원본 Toggle(): rainbow1 = (5,127,100) 즉시, rainbow2 = (228,125,43) 0.1초 뒤,
    // rainbow3 = (225,46,52) 0.2초 뒤로 순차 점등한다. rainbow4 는 색을 바꾸지
    // 않으므로 항상 Light(Main, 0.37) 그대로다.
    const ImU32 on[4] = {
        IM_COL32(5, 127, 100, 255), IM_COL32(228, 125, 43, 255),
        IM_COL32(225, 46, 52, 255), off,
    };
    auto stage = [&](int i) {
        return (i >= 3) ? 0.0f : ImClamp(lit * 3.0f - (float)i, 0.0f, 1.0f);
    };

    // 원본은 rainbow_1..4.png 4 장을 같은 자리에 겹쳐 놓는다.
    if (AssetsLoaded()) {
        const Asset layers[4] = { Asset::Rainbow1, Asset::Rainbow2,
                                  Asset::Rainbow3, Asset::Rainbow4 };
        bool drewAny = false;
        for (int i = 0; i < 4; i++)
            if (DrawAssetSized(dl, layers[i], tl, w, h, Mix(off, on[i], stage(i))))
                drewAny = true;
        if (drewAny) return;
    }

    float seg = w / 4.0f;
    for (int i = 0; i < 4; i++) {
        dl->AddRectFilled(ImVec2(tl.x + seg * i, tl.y),
                          ImVec2(tl.x + seg * (i + 1) - 1.0f, tl.y + h),
                          Mix(off, on[i], stage(i)), 1.0f);
    }
}

// colorpreview.png - 원본은 ImageColor3 = 색, ImageTransparency = 1 - Opacity
void ColorPreview(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float opacity) {
    ImVec2 a(c.x - s * 0.5f, c.y - s * 0.5f), b(c.x + s * 0.5f, c.y + s * 0.5f);
    ImVec4 v = V4(col); v.w = opacity;
    if (DrawAsset(dl, Asset::ColorPreview, a, b, U32(v))) return;

    dl->AddRectFilled(a, b, IM_COL32(60, 60, 62, 255), s * 0.5f);
    dl->AddRectFilled(a, b, U32(v), s * 0.5f);
}

// 원본 카테고리 정의 (new.lua:5766~) - 아이콘과 크기가 카테고리마다 다르다.
struct CatIconDef { const char* name; Asset asset; float w, h; };
const CatIconDef kCatIcons[] = {
    { "Combat",    Asset::CombatIcon,    13, 14 },
    { "Blatant",   Asset::BlatantIcon,   14, 14 },
    { "Render",    Asset::RenderIcon,    15, 14 },
    { "Utility",   Asset::UtilityIcon,   15, 14 },
    { "World",     Asset::WorldIcon,     14, 14 },
    { "Inventory", Asset::InventoryIcon, 15, 14 },
    { "Minigames", Asset::MiniIcon,      19, 12 },
    { "Targets",   Asset::TargetsTab,    18, 12 },
    // 나머지 아이콘과 마찬가지로 PNG 원본 픽셀 크기를 그대로 쓴다.
    { "Profiles",  Asset::ProfilesIcon,  17, 10 },
    { "Overlays",  Asset::OverlaysIcon,  24, 24 },
};

// 카테고리 이름 -> 아이콘 정의. 없으면 nullptr.
const CatIconDef* FindCatIcon(const std::string& name) {
    for (const auto& d : kCatIcons) if (name == d.name) return &d;
    return nullptr;
}

// 원본 PNG 로 카테고리 아이콘 그리기. 중심 기준.
// 실패 시 벡터 폴백(CatIcon)을 쓰도록 false 반환.
bool DrawCatIconTex(ImDrawList* dl, const std::string& name, ImVec2 center, ImU32 col) {
    const CatIconDef* d = FindCatIcon(name);
    if (!d) return false;
    return DrawAssetSized(dl, d->asset,
                          ImVec2(center.x - d->w * 0.5f, center.y - d->h * 0.5f),
                          d->w, d->h, col);
}

// 벡터 폴백 (PNG 로드 실패 시)
void CatIcon(ImDrawList* dl, int idx, ImVec2 c, float s, ImU32 col) {
    switch (idx) {
    case 0: // Combat - 검
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, 1.7f);
        dl->AddLine(ImVec2(c.x + s * 0.3f, c.y - s), ImVec2(c.x + s, c.y - s), col, 1.7f);
        dl->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x + s, c.y - s * 0.3f), col, 1.7f);
        break;
    case 1: // Blatant - 경고 삼각형
        dl->AddTriangleFilled(ImVec2(c.x, c.y - s), ImVec2(c.x - s, c.y + s),
                              ImVec2(c.x + s, c.y + s), col);
        break;
    case 2: // Render - 눈
        dl->AddCircle(c, s * 0.9f, col, 14, 1.5f);
        dl->AddCircleFilled(c, s * 0.35f, col, 8);
        break;
    case 3: // Utility - 렌치
        dl->AddRectFilled(ImVec2(c.x - s, c.y - 1.2f), ImVec2(c.x + s, c.y + 1.2f), col, 1.0f);
        dl->AddCircleFilled(ImVec2(c.x - s, c.y), 2.6f, col, 8);
        break;
    case 4: // World - 지구
        dl->AddCircle(c, s, col, 16, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), col, 1.1f);
        dl->AddBezierQuadratic(ImVec2(c.x, c.y - s), ImVec2(c.x - s * 0.75f, c.y),
                               ImVec2(c.x, c.y + s), col, 1.1f, 12);
        break;
    case 5: // Inventory - 상자
        dl->AddRect(ImVec2(c.x - s, c.y - s * 0.75f), ImVec2(c.x + s, c.y + s), col, 1.0f, 0, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y - s * 0.1f), ImVec2(c.x + s, c.y - s * 0.1f), col, 1.2f);
        break;
    case 6: // Targets - 조준선
        dl->AddCircle(c, s * 0.72f, col, 16, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y - s * 0.45f), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y + s * 0.45f), ImVec2(c.x, c.y + s), col, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.45f, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x + s * 0.45f, c.y), ImVec2(c.x + s, c.y), col, 1.5f);
        break;
    case 7: // Overlays - 겹친 창
        dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s * 0.35f, c.y + s * 0.35f), col, 1.0f, 0, 1.4f);
        dl->AddRect(ImVec2(c.x - s * 0.35f, c.y - s * 0.35f), ImVec2(c.x + s, c.y + s), col, 1.0f, 0, 1.4f);
        break;
    case 8: // Profiles - 사람
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.35f), s * 0.42f, col, 12);
        dl->AddBezierQuadratic(ImVec2(c.x - s * 0.85f, c.y + s), ImVec2(c.x, c.y - s * 0.15f),
                               ImVec2(c.x + s * 0.85f, c.y + s), col, 1.7f, 14);
        break;
    default: // Settings - 톱니
        dl->AddCircle(c, s * 0.5f, col, 12, 1.5f);
        for (int i = 0; i < 6; i++) {
            float a = (float)i * 3.14159265f / 3.0f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * s * 0.65f, c.y + std::sin(a) * s * 0.65f),
                        ImVec2(c.x + std::cos(a) * s, c.y + std::sin(a) * s), col, 1.5f);
        }
        break;
    }
}

// 지정 폰트로 텍스트 그리기 (원본의 TextSize 대응). font 가 nullptr 이면 기본.
void TextF(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* s) {
    if (font) dl->AddText(font, size, pos, col, s);
    else      dl->AddText(pos, col, s);
}

// 지정 폰트 기준 텍스트 크기
ImVec2 CalcTextF(ImFont* font, float size, const char* s) {
    if (font) return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
    return ImGui::CalcTextSize(s);
}

float EaseOutExpo(float t) {
    t = ImClamp(t, 0.0f, 1.0f);
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

std::string StripRichText(const std::string& text) {
    std::string plain;
    plain.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const size_t close = text.find('>', i + 1);
            if (close != std::string::npos) {
                std::string tag = text.substr(i + 1, close - i - 1);
                tag.erase(std::remove_if(tag.begin(), tag.end(),
                                         [](unsigned char c) { return std::isspace(c) != 0; }),
                          tag.end());
                std::transform(tag.begin(), tag.end(), tag.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (tag == "br/") plain.push_back('\n');
                i = close + 1;
                continue;
            }
        }
        plain.push_back(text[i++]);
    }
    return plain;
}

struct RichTextRun {
    std::string Text;
    ImU32 Color = IM_COL32_WHITE;
};

bool ParseRichColor(const std::string& tag, ImU32& color) {
    const size_t hash = tag.find('#');
    if (hash == std::string::npos || hash + 7 > tag.size()) return false;
    unsigned int rgb = 0;
    for (size_t i = hash + 1; i < hash + 7; i++) {
        const char c = tag[i];
        rgb <<= 4;
        if (c >= '0' && c <= '9') rgb |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') rgb |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') rgb |= (unsigned int)(c - 'A' + 10);
        else return false;
    }
    color = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
    return true;
}

std::vector<RichTextRun> ParseRichText(const std::string& text, ImU32 baseColor) {
    std::vector<RichTextRun> runs;
    std::vector<ImU32> colors{baseColor};
    auto append = [&](const std::string& value) {
        if (value.empty()) return;
        if (!runs.empty() && runs.back().Color == colors.back()) runs.back().Text += value;
        else runs.push_back({value, colors.back()});
    };

    for (size_t i = 0; i < text.size();) {
        const size_t open = text.find('<', i);
        if (open == std::string::npos) {
            append(text.substr(i));
            break;
        }
        append(text.substr(i, open - i));
        const size_t close = text.find('>', open + 1);
        if (close == std::string::npos) {
            append(text.substr(open));
            break;
        }

        std::string tag = text.substr(open + 1, close - open - 1);
        std::transform(tag.begin(), tag.end(), tag.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        std::string compact = tag;
        compact.erase(std::remove_if(compact.begin(), compact.end(),
                                     [](unsigned char c) { return std::isspace(c) != 0; }),
                      compact.end());
        if (compact == "br/") {
            append("\n");
        } else if (tag.rfind("/font", 0) == 0) {
            if (colors.size() > 1) colors.pop_back();
        } else if (tag.rfind("font", 0) == 0) {
            ImU32 parsed = colors.back();
            if (ParseRichColor(tag, parsed)) colors.push_back(parsed);
        }
        i = close + 1;
    }
    return runs;
}

void DrawRichText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                  const std::string& text, ImU32 baseColor) {
    float x = pos.x, y = pos.y;
    for (const auto& run : ParseRichText(text, baseColor)) {
        size_t start = 0;
        while (start <= run.Text.size()) {
            const size_t newline = run.Text.find('\n', start);
            const std::string part = newline == std::string::npos
                ? run.Text.substr(start) : run.Text.substr(start, newline - start);
            if (!part.empty()) {
                TextF(dl, font, size, ImVec2(x, y), run.Color, part.c_str());
                x += CalcTextF(font, size, part.c_str()).x;
            }
            if (newline == std::string::npos) break;
            x = pos.x;
            y += size;
            start = newline + 1;
        }
    }
}

// 폰트 높이 (세로 중앙 정렬용)
float FontH(ImFont* font, float size) {
    return font ? size : ImGui::GetFontSize();
}

// ---------------------------------------------------------------------------
// Roblox TextLabel 배치 재현
//
// 원본의 TextLabel/TextButton 은 지정한 사각형 안에서
//   - 세로: TextYAlignment 기본값 Center
//   - 가로: TextXAlignment (Left / Center / Right)
// 로 글자를 놓는다. 줄 높이는 TextSize 다.
// 따라서 "Position/Size 가 주어진 라벨" 은 전부 이 헬퍼로 그린다.
// ---------------------------------------------------------------------------
enum class XAlign { Left, Center, Right };

void TextIn(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImVec2 box,
            XAlign ax, ImU32 col, const char* s) {
    ImVec2 ts = CalcTextF(font, size, s);
    float x = pos.x;
    if (ax == XAlign::Center)      x = pos.x + (box.x - ts.x) * 0.5f;
    else if (ax == XAlign::Right)  x = pos.x + box.x - ts.x;
    TextF(dl, font, size, ImVec2(x, pos.y + (box.y - FontH(font, size)) * 0.5f), col, s);
}

// Enum.TextTruncate.AtEnd - 폭을 넘으면 뒤를 잘라내고 '...' 을 붙인다.
std::string Truncate(ImFont* font, float size, const std::string& s, float maxW) {
    if (CalcTextF(font, size, s.c_str()).x <= maxW) return s;
    const float dotsW = CalcTextF(font, size, "...").x;
    if (dotsW > maxW) return "";
    size_t n = s.size();
    while (n > 0) {
        std::string cut = s.substr(0, n - 1) + "...";
        if (CalcTextF(font, size, cut.c_str()).x <= maxW) return cut;
        n--;
    }
    return "...";
}

// 원본은 아이콘 자리를 일반 공백이 아니라 U+200A HAIR SPACE 로 확보한다.
//   modulebutton.Text = <U+200A>x12 .. Name   (TextSize 14)
// 폰트에 그 글리프가 있으면 실제 폭으로 계산하고, 없으면 폴백값을 쓴다.
float HairIndent(ImFont* font, float size, int count, float fallbackPx) {
    if (font && font->FontSize > 0.0f) {
        if (const ImFontGlyph* g = font->FindGlyphNoFallback((ImWchar)0x200A))
            return (float)count * g->AdvanceX * (size / font->FontSize);
    }
    return fallbackPx;
}

// mainapi:TextColor (원본 448) - 밝은 강조색 위에서는 어두운 글자를 쓴다.
ImU32 TextOnAccent(float h, float s, float v) {
    if (v >= 0.7f && (s < 0.6f || (h > 0.04f && h < 0.56f)))
        return IM_COL32(48, 48, 48, 255);   // Color3.new(0.19, 0.19, 0.19)
    return IM_COL32(255, 255, 255, 255);
}

// 'Show tooltips' 설정 (원본 guipane 의 토글). Tip() 이 참조한다.
bool g_showTooltips = true;

// 원본 discordbutton (gui.lua:1666) 은 Discord RPC 로 초대 코드 VZEQJxMSnG 를
// 띄운다. 데스크톱 앱에서는 같은 초대 링크를 기본 브라우저로 연다.
void OpenDiscordInvite() {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", "https://discord.gg/VZEQJxMSnG",
                  nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

// addTooltip - 원본은 마우스 옆 16px 에 표시
void Tip(const char* text) {
    if (!g_showTooltips) return;
    if (!text || !*text) return;
    if (!ImGui::IsItemHovered()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Dark(g_palette.Main, 0.02f));
    ImGui::PushStyleColor(ImGuiCol_Text, g_palette.Text);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// 창 너비 - 원본은 전부 220px 고정
constexpr float kWinW = 220.0f;

// 숫자를 원본처럼 표기 (Lua tostring: 정수는 소수점 없음)
std::string NumStr(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.0001f)
        std::snprintf(buf, sizeof(buf), "%d", (int)std::round(v));
    else
        std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// TextSize 14 용 폰트. Arial 로드에 실패했으면 nullptr 을 돌려주어
// TextF / HairIndent 가 폴백 경로(ImGui 기본 폰트 + 픽셀 상수)를 타게 한다.
ImFont* Font14() { return g_fontSmall ? ImGui::GetFont() : nullptr; }

// HAIR SPACE 들여쓰기 개수 (원본 그대로)
constexpr int kToggleHair = 10;     // Toggle.lua:15,       TextSize 14
constexpr int kModuleHair = 12;     // gui.lua:1820,        TextSize 14
constexpr int kDropdownHair = 9;    // Dropdown.lua:36,     TextSize 13
constexpr int kCatHair = 33;        // gui.lua:780 (아이콘 있음),  TextSize 14
constexpr int kCatHairNoIcon = 13;  // gui.lua:780 (아이콘 없음),  TextSize 14

// 폴백 계수 0.0886:
//   Windows 의 arial.ttf 에는 U+200A 글리프가 없어서 Roblox 도 폴백 폰트로 그린다.
//   HAIR SPACE 를 em/10 으로 잡는 폰트가 흔하고, Roblox 의 TextSize 는 줄 높이
//   (Arial 기준 1.117 em) 이므로 TextSize / 1.117 / 10 = TextSize * 0.0895 이다.
//   이 값으로 계산하면 아이콘 있는 카테고리 버튼의 33칸이 약 41px 이 되어, 같은
//   자리의 카테고리 창 제목 x(40) 과 일치한다.
constexpr float kHairPerPx = 0.0886f;
constexpr float kToggleTextFallback = kToggleHair * 14.0f * kHairPerPx;
constexpr float kModuleTextFallback = kModuleHair * 14.0f * kHairPerPx;
constexpr float kDropdownTextFallback = kDropdownHair * 13.0f * kHairPerPx;

// ---------------------------------------------------------------------------
// addBlur - 원본은 창 뒤에 blur.png 를 깔아 부드러운 그림자를 만든다.
// blur.png(309x533) 실측값:
//   그림자 폭 13px, 가장 진한 곳 alpha 70, 색 (0,0,0)
//   콘텐츠 영역      x 48..267 (= 창 폭 220), y 33..511
//   그림자 포함 영역 x 35..280 (246), y 20..524 (505)
// 창보다 뒤에 깔려야 하므로 배경 드로우리스트에 그린다.
// ---------------------------------------------------------------------------
void Blur(ImVec2 min, ImVec2 max) {
    if (!g_blurEnabled) return;
    // Size = (1, 89),(1, 52) / Position = (-48, -31)
    //   -> 좌 48, 상 31, 우 89-48 = 41, 하 52-31 = 21 만큼 창보다 크다
    // SliceCenter = Rect(52, 31, 261, 502) -> 테두리 L52 T31 R48 B31
    DrawAssetSliced(ImGui::GetBackgroundDrawList(), Asset::Blur,
                    ImVec2(min.x - 48.0f, min.y - 31.0f),
                    ImVec2(max.x + 41.0f, max.y + 21.0f),
                    0.0f, 0.0f, 309.0f, 533.0f,
                    52.0f, 31.0f, 48.0f, 31.0f,
                    IM_COL32(255, 255, 255, 255));
}

// addCloseButton (원본 gui.lua:165)
//   24x24 @ (1,-35),(0,offset or 9), close.png
//   ImageColor3 = Light(Text, 0.2), ImageTransparency 0.5 -> 0.3 (호버)
//   BackgroundColor3 흰색, BackgroundTransparency 1 -> 0.6 (호버), 모서리 완전 둥금
bool CloseButton(ImDrawList* dl, ImVec2 winPos, float winW, float offset = 9.0f) {
    ImVec2 c(winPos.x + winW - 35.0f, winPos.y + offset);
    ImGui::SetCursorScreenPos(c);
    ImGui::InvisibleButton("##close", ImVec2(24, 24));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float bt = Tween01(ImGui::GetID("##closebg"), hovered);
    dl->AddRectFilled(c, ImVec2(c.x + 24.0f, c.y + 24.0f),
                      IM_COL32(255, 255, 255, (int)(bt * 0.4f * 255.0f)), 12.0f);
    ImVec4 col = V4(Light(g_palette.Text, 0.2f));
    col.w = hovered ? 0.7f : 0.5f;
    if (!DrawAssetSized(dl, Asset::Close, c, 24.0f, 24.0f, U32(col)))
        Cross(dl, ImVec2(c.x + 12.0f, c.y + 12.0f), 24.0f, U32(col));
    return clicked;
}

// ---------------------------------------------------------------------------
// components.Toggle - 높이 30, 이름은 공백 10칸 들여쓰기(TextSize 14),
// knobholder 22x12 @ (1,-30),(0,9), knob 8x8 (x = 2 / 12).
// Targets 창 안에서도 같은 컴포넌트를 쓰므로 따로 뺀다.
// 반환값: 이번 프레임에 눌렸는지.
// ---------------------------------------------------------------------------
bool ToggleRow(ImVec2 origin, float width, const char* name, bool& value,
               ImU32 rowBg, int index, const char* tooltip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 30.0f;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), rowBg);

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##tg", ImVec2(width, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    if (clicked) value = !value;
    if (tooltip) Tip(tooltip);

    TextIn(dl, Font14(), 14.0f,
           ImVec2(origin.x + HairIndent(Font14(), 14.0f, kToggleHair, kToggleTextFallback), origin.y),
           ImVec2(width, h), XAlign::Left, Dark(g_palette.Text, 0.16f), name);

    ImVec2 khMin(origin.x + width - 30.0f, origin.y + 9.0f);
    ImVec2 khMax(khMin.x + 22.0f, khMin.y + 12.0f);
    // 원본 색: 켜짐 = 강조색 / 꺼짐 = 호버시 Light 0.37, 아니면 Light 0.14
    ImU32 target = value ? g_palette.AccentIndexed(index)
                         : (hovered ? Light(g_palette.Main, 0.37f)
                                    : Light(g_palette.Main, 0.14f));
    dl->AddRectFilled(khMin, khMax, target, 6.0f);   // UDim.new(1,0) = 완전 둥금

    float kx = TweenVal(ImGui::GetID("##tgknob"), value ? 12.0f : 2.0f,
                        g_palette.Tween, 10.0f);
    dl->AddRectFilled(ImVec2(khMin.x + kx, khMin.y + 2.0f),
                      ImVec2(khMin.x + kx + 8.0f, khMin.y + 10.0f),
                      g_palette.Main, 4.0f);
    return clicked;
}

} // namespace

// ===========================================================================
// 키바인드
// ===========================================================================

namespace {

// Win32 비동기 상태를 같이 보관해 창 포커스가 없어도 bind가 작동한다.
struct BindingKey {
    int vk;
    ImGuiKey key;
    std::string name;
    bool mouse = false;
};

const std::vector<BindingKey>& BindingKeys() {
    static const std::vector<BindingKey> keys = [] {
        std::vector<BindingKey> out = {
            {VK_TAB,ImGuiKey_Tab,"Tab"}, {VK_LEFT,ImGuiKey_LeftArrow,"Left"},
            {VK_RIGHT,ImGuiKey_RightArrow,"Right"}, {VK_UP,ImGuiKey_UpArrow,"Up"},
            {VK_DOWN,ImGuiKey_DownArrow,"Down"}, {VK_PRIOR,ImGuiKey_PageUp,"PageUp"},
            {VK_NEXT,ImGuiKey_PageDown,"PageDown"}, {VK_HOME,ImGuiKey_Home,"Home"},
            {VK_END,ImGuiKey_End,"End"}, {VK_INSERT,ImGuiKey_Insert,"Insert"},
            {VK_DELETE,ImGuiKey_Delete,"Delete"}, {VK_BACK,ImGuiKey_Backspace,"Backspace"},
            {VK_SPACE,ImGuiKey_Space,"Space"}, {VK_RETURN,ImGuiKey_Enter,"Return"},
            {VK_ESCAPE,ImGuiKey_Escape,"Escape"}, {VK_LCONTROL,ImGuiKey_LeftCtrl,"LeftControl"},
            {VK_LSHIFT,ImGuiKey_LeftShift,"LeftShift"}, {VK_LMENU,ImGuiKey_LeftAlt,"LeftAlt"},
            {VK_LWIN,ImGuiKey_LeftSuper,"LeftSuper"}, {VK_RCONTROL,ImGuiKey_RightCtrl,"RightControl"},
            {VK_RSHIFT,ImGuiKey_RightShift,"RightShift"}, {VK_RMENU,ImGuiKey_RightAlt,"RightAlt"},
            {VK_RWIN,ImGuiKey_RightSuper,"RightSuper"}, {VK_OEM_7,ImGuiKey_Apostrophe,"Quote"},
            {VK_OEM_COMMA,ImGuiKey_Comma,"Comma"}, {VK_OEM_MINUS,ImGuiKey_Minus,"Minus"},
            {VK_OEM_PERIOD,ImGuiKey_Period,"Period"}, {VK_OEM_2,ImGuiKey_Slash,"Slash"},
            {VK_OEM_1,ImGuiKey_Semicolon,"Semicolon"}, {VK_OEM_PLUS,ImGuiKey_Equal,"Equals"},
            {VK_OEM_4,ImGuiKey_LeftBracket,"LeftBracket"}, {VK_OEM_5,ImGuiKey_Backslash,"BackSlash"},
            {VK_OEM_6,ImGuiKey_RightBracket,"RightBracket"}, {VK_OEM_3,ImGuiKey_GraveAccent,"Backquote"},
            {VK_CAPITAL,ImGuiKey_CapsLock,"CapsLock"}, {VK_NUMLOCK,ImGuiKey_NumLock,"NumLock"},
            {VK_SNAPSHOT,ImGuiKey_PrintScreen,"PrintScreen"}, {VK_PAUSE,ImGuiKey_Pause,"Pause"},
            {VK_DECIMAL,ImGuiKey_KeypadDecimal,"KeypadPeriod"},
            {VK_DIVIDE,ImGuiKey_KeypadDivide,"KeypadDivide"},
            {VK_MULTIPLY,ImGuiKey_KeypadMultiply,"KeypadMultiply"},
            {VK_SUBTRACT,ImGuiKey_KeypadSubtract,"KeypadMinus"},
            {VK_ADD,ImGuiKey_KeypadAdd,"KeypadPlus"},
            {VK_RBUTTON,ImGuiKey_MouseRight,"Mouse2",true},
            {VK_MBUTTON,ImGuiKey_MouseMiddle,"Mouse3",true},
            {VK_XBUTTON1,ImGuiKey_MouseX1,"Mouse4",true},
            {VK_XBUTTON2,ImGuiKey_MouseX2,"Mouse5",true},
        };
        for (int i = 0; i < 26; i++)
            out.push_back({'A' + i, (ImGuiKey)(ImGuiKey_A + i), std::string(1, (char)('A' + i))});
        for (int i = 0; i < 10; i++)
            out.push_back({'0' + i, (ImGuiKey)(ImGuiKey_0 + i), std::string(1, (char)('0' + i))});
        static const char* keypadNames[10] = {
            "KeypadZero","KeypadOne","KeypadTwo","KeypadThree","KeypadFour",
            "KeypadFive","KeypadSix","KeypadSeven","KeypadEight","KeypadNine"
        };
        for (int i = 0; i < 10; i++)
            out.push_back({VK_NUMPAD0 + i, (ImGuiKey)(ImGuiKey_Keypad0 + i), keypadNames[i]});
        for (int i = 0; i < 24; i++)
            out.push_back({VK_F1 + i, (ImGuiKey)(ImGuiKey_F1 + i), "F" + std::to_string(i + 1)});
        return out;
    }();
    return keys;
}

// 원본 checkKeybinds(compare, target, key):
// key 가 target 에 있고, target 의 모든 키가 compare(현재 눌린 키)에 있으면 true.
bool CheckKeybinds(const std::vector<std::string>& held,
                   const std::vector<std::string>& target,
                   const std::string& key) {
    if (target.empty()) return false;
    if (std::find(target.begin(), target.end(), key) == target.end()) return false;
    for (const auto& t : target)
        if (std::find(held.begin(), held.end(), t) == held.end()) return false;
    return true;
}

std::string JoinUpper(const std::vector<std::string>& keys) {
    std::string s;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) s += " + ";
        for (char c : keys[i]) s += (char)std::toupper((unsigned char)c);
    }
    return s;
}

bool ExactKeybind(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return false;
    std::vector<std::string> aa = a, bb = b;
    std::sort(aa.begin(), aa.end());
    std::sort(bb.begin(), bb.end());
    return aa == bb;
}

} // namespace

void VapeGui::BeginModuleBinding(Module* mod) {
    if (!mod) return;
    CancelBinding();
    BindingModule = mod;
    BindingGui = false;
    bindingKeys_.clear();
    bindingLastKey_.clear();
    HeldKeys.clear();
    mod->BindCoverText = "PRESS A KEY TO BIND";
    mod->BindCoverTimer = 0.0f;
}

void VapeGui::BeginGuiBinding() {
    CancelBinding();
    BindingModule = nullptr;
    BindingGui = true;
    bindingKeys_.clear();
    bindingLastKey_.clear();
    HeldKeys.clear();
}

void VapeGui::CancelBinding() {
    if (BindingModule) {
        BindingModule->BindCoverText = "BIND CANCELLED";
        BindingModule->BindCoverTimer = 0.75f;
    }
    BindingModule = nullptr;
    BindingGui = false;
    bindingKeys_.clear();
    bindingLastKey_.clear();
}

void VapeGui::FinishBinding(const std::vector<std::string>& keys) {
    if (BindingModule) {
        Module* mod = BindingModule;
        mod->Bind = ExactKeybind(mod->Bind, keys) ? std::vector<std::string>{} : keys;
        mod->BindCoverText = mod->Bind.empty() ? "BIND REMOVED" : "BOUND TO";
        mod->BindCoverTimer = 1.0f;
    } else if (BindingGui && !keys.empty() && !ExactKeybind(Keybind, keys)) {
        Keybind = keys;
    }
    BindingModule = nullptr;
    BindingGui = false;
    bindingKeys_.clear();
    bindingLastKey_.clear();
    Save();
}

void VapeGui::ProcessKeybinds() {
    ImGuiIO& io = ImGui::GetIO();
    const auto& keys = BindingKeys();
    if (previousKeyDown_.size() != keys.size()) {
        previousKeyDown_.assign(keys.size(), 0);
        keyStatesInitialized_ = false;
    }
    std::vector<unsigned char> down(keys.size(), 0);
    for (size_t i = 0; i < keys.size(); i++) {
#ifdef _WIN32
        down[i] = (::GetAsyncKeyState(keys[i].vk) & 0x8000) != 0;
#else
        down[i] = ImGui::IsKeyDown(keys[i].key);
#endif
    }

    for (auto& cat : Categories)
        for (auto& mod : cat->Modules)
            if (mod->BindCoverTimer > 0.0f) mod->BindCoverTimer -= io.DeltaTime;

    if (!keyStatesInitialized_) {
        previousKeyDown_ = down;
        keyStatesInitialized_ = true;
        return;
    }

    const bool typing = io.WantTextInput;
    for (size_t i = 0; i < keys.size(); i++) {
        const bool pressed = down[i] && !previousKeyDown_[i];
        const bool released = !down[i] && previousKeyDown_[i];
        const std::string& name = keys[i].name;

        if (pressed) {
            if (IsBinding()) {
                if (name == "Escape") {
                    CancelBinding();
                    continue;
                }
                if ((name == "Delete" || name == "Backspace") && BindingModule) {
                    FinishBinding({});
                    continue;
                }
                if (std::find(bindingKeys_.begin(), bindingKeys_.end(), name) == bindingKeys_.end())
                    bindingKeys_.push_back(name);
                bindingLastKey_ = name;
                continue;
            }
            if (typing || (keys[i].mouse && io.WantCaptureMouse)) continue;
            if (std::find(HeldKeys.begin(), HeldKeys.end(), name) == HeldKeys.end())
                HeldKeys.push_back(name);

            if (CheckKeybinds(HeldKeys, Keybind, name)) {
                Visible = !Visible;
            }
            for (auto& cat : Categories)
                for (auto& mod : cat->Modules)
                    if (CheckKeybinds(HeldKeys, mod->Bind, name)) ToggleModule(mod.get(), true);
        }

        if (released) {
            if (IsBinding()) {
                if (!MultiKeybind && name == bindingLastKey_) {
                    FinishBinding({name});
                } else if (MultiKeybind && !bindingKeys_.empty()) {
                    bool capturedStillDown = false;
                    for (size_t j = 0; j < keys.size(); j++)
                        if (down[j] && std::find(bindingKeys_.begin(), bindingKeys_.end(),
                                                 keys[j].name) != bindingKeys_.end()) {
                            capturedStillDown = true;
                            break;
                        }
                    if (!capturedStillDown) FinishBinding(bindingKeys_);
                }
            }
            auto it = std::find(HeldKeys.begin(), HeldKeys.end(), name);
            if (it != HeldKeys.end()) HeldKeys.erase(it);
        }
    }
    previousKeyDown_ = std::move(down);
}

// ===========================================================================
// 옵션 생성
// ===========================================================================

Option* Module::CreateToggle(const std::string& name, bool def, const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::Toggle;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Enabled = def;
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateSlider(const std::string& name, float min, float max, float def,
                             float decimal, const std::string& suffix,
                             const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::Slider;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Min = min; o->Max = max; o->Value = def;
    o->Decimal = decimal; o->Suffix = suffix;
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateTwoSlider(const std::string& name, float min, float max,
                                float defMin, float defMax, float decimal,
                                const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::TwoSlider;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Min = min; o->Max = max;
    o->ValueMin = defMin; o->ValueMax = defMax;
    o->Decimal = decimal;
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateDropdown(const std::string& name, const std::vector<std::string>& list,
                               const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::Dropdown;
    o->Name = name;
    o->Tooltip = tooltip.empty() ? name : tooltip;   // 원본: Tooltip or Name
    o->List = list;
    o->Selected = list.empty() ? "None" : list[0];
    o->Index = 0;   // 원본 Dropdown.lua:4 - Index 는 0 고정
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateTextBox(const std::string& name, const std::string& placeholder,
                              const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::TextBox;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Placeholder = placeholder;
    o->Index = 0;   // 원본 TextBox.lua:4 - Index 는 0 고정
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateColorSlider(const std::string& name, const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::ColorSlider;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Index = 0;   // 원본 ColorSlider.lua:8 - Index 는 0 고정
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateButton(const std::string& name, std::function<void()> fn,
                             const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::Button;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Function = std::move(fn);
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateTextList(const std::string& name, const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::TextList;
    o->Name = name;
    o->Tooltip = tooltip;
    o->Placeholder = "Add entry...";
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

Option* Module::CreateTargets(const std::string& tooltip) {
    auto o = std::make_unique<Option>();
    o->Type = OptionType::Targets;
    o->Name = "Targets";
    o->Tooltip = tooltip;
    o->Index = (int)Options.size();
    Options.push_back(std::move(o));
    return Options.back().get();
}

void Module::Toggle() {
    Enabled = !Enabled;
    if (Function) Function(Enabled);
}

// ===========================================================================
// 카테고리
// ===========================================================================

Module* Category::CreateModule(const std::string& name, std::function<void(bool)> fn,
                               const std::string& tooltip) {
    auto m = std::make_unique<Module>();
    m->Name = name;
    m->Tooltip = tooltip;
    m->Function = std::move(fn);
    // 원본: Index = getTableSize(mainapi.Modules) - 카테고리와 무관한 전체 생성 순서.
    // 무지개 색 오프셋에만 쓰이므로 정렬의 영향을 받지 않는다.
    static int s_moduleCounter = 0;
    m->Index = s_moduleCounter++;
    Modules.push_back(std::move(m));

    // 표시 순서는 UIListLayout(SortOrder.LayoutOrder) 이 이름순으로 정렬한다.
    std::sort(Modules.begin(), Modules.end(),
              [](const std::unique_ptr<Module>& a, const std::unique_ptr<Module>& b) {
                  return a->Name < b->Name;
              });

    for (auto& m2 : Modules) if (m2->Name == name) return m2.get();
    return nullptr;
}

void Category::Expand() { Expanded = !Expanded; }

Category* VapeGui::CreateCategory(const std::string& name) {
    auto c = std::make_unique<Category>();
    c->Name = name;
    c->IconIndex = (int)Categories.size();
    // 원본 window.Position = UDim2.fromOffset(236, 60)
    c->Position = ImVec2(236.0f, 60.0f);
    Categories.push_back(std::move(c));
    return Categories.back().get();
}

// ===========================================================================
// 옵션 렌더링
// ===========================================================================

void VapeGui::RenderOption(Option* opt, float width, float& y, bool darker, ImU32 childrenBg) {
    if (!opt->Visible) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 origin(wp.x, wp.y + y);

    // 원본: color.Dark(children.BackgroundColor3, Darker and 0.02 or 0)
    ImU32 rowBg = Dark(childrenBg, (darker || opt->Darker) ? 0.02f : 0.0f);
    ImU32 dimText = Dark(g_palette.Text, 0.16f);

    ImGui::PushID(opt);

    switch (opt->Type) {

    // -----------------------------------------------------------------------
    // Toggle - 원본 2084
    //   높이 30, 텍스트 앞 공백 10칸, TextSize 14
    //   knobholder 22x12 @ (1,-30),(0,9),  knob 8x8 @ (2,2) -> (12,2)
    // -----------------------------------------------------------------------
    case OptionType::Toggle: {
        if (ToggleRow(origin, width, opt->Name.c_str(), opt->Enabled, rowBg, opt->Index,
                      opt->Tooltip.c_str()))
            FireOption(opt, true);
        y += 30.0f;
        break;
    }

    // -----------------------------------------------------------------------
    // Slider - 원본 1156
    //   높이 50, title 11px @ (10,2), value 우측 @ (width-69, 9)
    //   트랙 2px @ (10,37), fill clamp 0.04~0.96
    //   knobholder 24x4, knob 14 -> 호버 16
    // -----------------------------------------------------------------------
    case OptionType::Slider: {
        const float h = 50.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImVec2 trackMin(origin.x + 10.0f, origin.y + 37.0f);
        float  trackW = width - 20.0f;

        // 원본: 클릭 Y 가 상단 20px 아래일 때만 드래그
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 20.0f));
        ImGui::InvisibleButton("##sl", ImVec2(width, h - 20.0f));
        bool held = ImGui::IsItemActive();
        Tip(opt->Tooltip.c_str());
        // 노브 확대는 slider.MouseEnter/MouseLeave 기준이므로 행 전체(50px)가 대상이다.
        bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                       ImGui::IsMouseHoveringRect(bb.Min, bb.Max);

        if (held) {
            float p = ImClamp((ImGui::GetIO().MousePos.x - trackMin.x) / trackW, 0.0f, 1.0f);
            // 원본: floor((Min + (Max-Min)*p) * Decimal) / Decimal
            float nv = std::floor((opt->Min + (opt->Max - opt->Min) * p) * opt->Decimal) / opt->Decimal;
            if (nv != opt->Value) {
                opt->Value = nv;
                FireOption(opt, false);
            }
            opt->Touched = true;
        }
        if (opt->InteractionActive && !held) FireOption(opt, true);
        opt->InteractionActive = held;

        // title: Size (60,30) @ (10,2), TextSize 11, 왼쪽 정렬 + 세로 중앙
        TextIn(dl, g_fontSmall, 11.0f, ImVec2(origin.x + 10.0f, origin.y + 2.0f),
               ImVec2(60.0f, 30.0f), XAlign::Left, dimText, opt->Name.c_str());

        // 값 텍스트 (원본: Value .. ' ' .. Suffix)
        std::string vs = NumStr(opt->Value);
        if (!opt->Suffix.empty()) vs += " " + opt->Suffix;

        // valuebutton: Size (60,15) @ (1,-69),(0,9), 오른쪽 정렬 + 세로 중앙
        ImVec2 vbMin(origin.x + width - 69.0f, origin.y + 9.0f);
        ImGui::SetCursorScreenPos(vbMin);
        ImGui::InvisibleButton("##vb", ImVec2(60, 15));
        if (ImGui::IsItemClicked()) {
            opt->BoxEditing = true;
            std::snprintf(opt->EditBuffer, sizeof(opt->EditBuffer), "%s", NumStr(opt->Value).c_str());
        }

        if (opt->BoxEditing) {
            ImGui::SetCursorScreenPos(ImVec2(vbMin.x, vbMin.y - 2.0f));
            ImGui::SetNextItemWidth(60.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##e", opt->EditBuffer, sizeof(opt->EditBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                float parsed = 0.0f;
                if (ParseFiniteFloat(opt->EditBuffer, parsed)) {
                    opt->Value = ImClamp(parsed, opt->Min, opt->Max);
                    opt->BoxEditing = false;
                    opt->Touched = true;
                    FireOption(opt, true);
                }
            } else if (ImGui::IsItemDeactivated()) {
                opt->BoxEditing = false;
            }
            ImGui::PopStyleColor(2);
        } else {
            TextIn(dl, g_fontSmall, 11.0f, vbMin, ImVec2(60.0f, 15.0f),
                   XAlign::Right, dimText, vs.c_str());
        }

        // 트랙 - 원본 bkg = Light(Main, 0.034)
        dl->AddRectFilled(trackMin, ImVec2(trackMin.x + trackW, trackMin.y + 2.0f),
                          Light(g_palette.Main, 0.034f));
        // 원본은 생성 시 (Value - Min) / Max, SetValue 이후로는 Value / Max 를 쓴다.
        float frac = 0.0f;
        if (opt->Max != 0.0f)
            frac = ImClamp(opt->Touched ? opt->Value / opt->Max
                                        : (opt->Value - opt->Min) / opt->Max, 0.0f, 1.0f);
        float df = ImClamp(frac, 0.04f, 0.96f);
        ImU32 accent = g_palette.AccentIndexed(opt->Index);
        dl->AddRectFilled(trackMin, ImVec2(trackMin.x + trackW * df, trackMin.y + 2.0f), accent);

        // knobholder 24x4 (배경색으로 트랙을 가림), knob 14 -> 16
        float ks = TweenVal(ImGui::GetID("##slk"), (hovered || held) ? 16.0f : 14.0f,
                            g_palette.Tween, 2.0f);
        ImVec2 kc(trackMin.x + trackW * df, trackMin.y + 1.0f);
        dl->AddRectFilled(ImVec2(kc.x - 12.0f, kc.y - 2.0f), ImVec2(kc.x + 12.0f, kc.y + 2.0f), rowBg);
        dl->AddCircleFilled(kc, ks * 0.5f, accent, 24);
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // TwoSlider - 원본 2177
    //   높이 50, value 우측 @ (width-69,9), value2 @ (width-125,9)
    //   arrow 12x6 @ (width-56,10),  knob = range.png 9x16 -> 11x18
    //   fill.Position = min, fill.Size = max - min
    // -----------------------------------------------------------------------
    case OptionType::TwoSlider: {
        const float h = 50.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImVec2 trackMin(origin.x + 10.0f, origin.y + 37.0f);
        float  trackW = width - 20.0f;

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 20.0f));
        ImGui::InvisibleButton("##ts", ImVec2(width, h - 20.0f));
        bool held = ImGui::IsItemActive();
        Tip(opt->Tooltip.c_str());
        // 노브 확대는 행 전체 호버 기준 (slider.MouseEnter)
        bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                       ImGui::IsMouseHoveringRect(bb.Min, bb.Max);

        float posMin = ImClamp(ImClamp(opt->ValueMin / opt->Max, 0.0f, 1.0f), 0.04f, 0.96f);
        float posMax = ImClamp(ImClamp(opt->ValueMax / opt->Max, 0.0f, 1.0f), 0.04f, 0.96f);

        // 원본: maxCheck = (mouseX - knobMax.AbsolutePosition.X) > -10
        static Option* s_active = nullptr;
        static bool    s_maxCheck = false;
        if (held) {
            float knobMaxX = trackMin.x + trackW * posMax;
            if (s_active != opt) {
                s_active = opt;
                s_maxCheck = (ImGui::GetIO().MousePos.x - knobMaxX) > -10.0f;
            }
            float p = ImClamp((ImGui::GetIO().MousePos.x - trackMin.x) / trackW, 0.0f, 1.0f);
            float nv = std::floor((opt->Min + (opt->Max - opt->Min) * p) * opt->Decimal) / opt->Decimal;
            const float beforeMin = opt->ValueMin, beforeMax = opt->ValueMax;
            if (s_maxCheck) opt->ValueMax = std::max(nv, opt->ValueMin);
            else            opt->ValueMin = std::min(nv, opt->ValueMax);
            if (beforeMin != opt->ValueMin || beforeMax != opt->ValueMax)
                FireOption(opt, false);
            posMin = ImClamp(ImClamp(opt->ValueMin / opt->Max, 0.0f, 1.0f), 0.04f, 0.96f);
            posMax = ImClamp(ImClamp(opt->ValueMax / opt->Max, 0.0f, 1.0f), 0.04f, 0.96f);
        } else if (s_active == opt) {
            s_active = nullptr;
        }
        if (opt->InteractionActive && !held) FireOption(opt, true);
        opt->InteractionActive = held;

        // title: Size (60,30) @ (10,2), TextSize 11
        TextIn(dl, g_fontSmall, 11.0f, ImVec2(origin.x + 10.0f, origin.y + 2.0f),
               ImVec2(60.0f, 30.0f), XAlign::Left, dimText, opt->Name.c_str());

        // 두 값은 클릭 후 직접 입력할 수 있다.
        auto rangeValue = [&](const char* id, ImVec2 pos, bool isMax) {
            bool& editing = isMax ? opt->BoxEditing : opt->BoxEditing2;
            float& value = isMax ? opt->ValueMax : opt->ValueMin;
            ImGui::PushID(id);
            ImGui::SetCursorScreenPos(pos);
            ImGui::InvisibleButton("##rangevalue", ImVec2(isMax ? 60.0f : 52.0f, 15.0f));
            if (ImGui::IsItemClicked()) {
                opt->BoxEditing = isMax;
                opt->BoxEditing2 = !isMax;
                editing = true;
                std::snprintf(opt->EditBuffer, sizeof(opt->EditBuffer), "%s", NumStr(value).c_str());
            }
            if (editing) {
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y - 2.0f));
                ImGui::SetNextItemWidth(isMax ? 60.0f : 52.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, dimText);
                if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive())
                    ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rangeedit", opt->EditBuffer, sizeof(opt->EditBuffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    float parsed = 0.0f;
                    if (ParseFiniteFloat(opt->EditBuffer, parsed)) {
                        value = isMax ? ImClamp(parsed, opt->ValueMin, opt->Max)
                                      : ImClamp(parsed, opt->Min, opt->ValueMax);
                        editing = false;
                        FireOption(opt, true);
                    }
                } else if (ImGui::IsItemDeactivated()) {
                    editing = false;
                }
                ImGui::PopStyleColor(2);
            } else {
                const std::string text = NumStr(value);
                TextIn(dl, g_fontSmall, 11.0f, pos, ImVec2(isMax ? 60.0f : 52.0f, 15.0f),
                       XAlign::Right, dimText, text.c_str());
            }
            ImGui::PopID();
        };
        rangeValue("max", ImVec2(origin.x + width - 69.0f, origin.y + 9.0f), true);
        rangeValue("min", ImVec2(origin.x + width - 125.0f, origin.y + 9.0f), false);

        // rangearrow.png 12x6 @ (width-56, 10)
        // rangearrow.png 12x6 @ (1,-56),(0,10)
        if (!DrawAssetSized(dl, Asset::RangeArrow,
                            ImVec2(origin.x + width - 56.0f, origin.y + 10.0f),
                            12.0f, 6.0f, Light(g_palette.Main, 0.14f)))
            Arrow(dl, ImVec2(origin.x + width - 50.0f, origin.y + 13.0f), 4.0f, 8.0f, 0.0f,
                  Light(g_palette.Main, 0.14f));

        // 트랙 + fill
        dl->AddRectFilled(trackMin, ImVec2(trackMin.x + trackW, trackMin.y + 2.0f),
                          Light(g_palette.Main, 0.034f));
        ImU32 accent = g_palette.AccentIndexed(opt->Index);
        dl->AddRectFilled(ImVec2(trackMin.x + trackW * posMin, trackMin.y),
                          ImVec2(trackMin.x + trackW * posMax, trackMin.y + 2.0f), accent);

        // range.png 노브 2개 (9x16, 호버시 11x18).
        // 원본의 확대 판정은 각 knobholder(16x4) 위에서만 일어난다.
        float cy = trackMin.y + 1.0f;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto knobHover = [&](float cx) {
            return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                   mouse.x >= cx - 8.0f && mouse.x <= cx + 8.0f &&
                   mouse.y >= cy - 2.0f && mouse.y <= cy + 2.0f;
        };
        auto drawKnob = [&](const char* id, float pos, bool flip) {
            float cx = trackMin.x + trackW * pos;
            float kw = TweenVal(ImGui::GetID(id), knobHover(cx) ? 11.0f : 9.0f,
                                g_palette.Tween, 2.0f);
            float kh = kw * (16.0f / 9.0f);
            // knobholder 16x4 가 배경색으로 트랙을 가린다.
            dl->AddRectFilled(ImVec2(cx - 8.0f, cy - 2.0f), ImVec2(cx + 8.0f, cy + 2.0f), rowBg);
            RangeKnob(dl, ImVec2(cx, cy), kw, kh, flip, accent);
        };
        drawKnob("##tskmin", posMin, false);
        drawKnob("##tskmax", posMax, true);
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // Dropdown - 원본 953
    //   접힘 40, 펼침 40 + (개수-1)*26
    //   bkg = (1,-20),(1,-9) @ (10,4), corner 6
    //   title = '         ' .. Name .. ' - ' .. Value, TextSize 13
    //   arrow 4x8 @ (1,-17),(0,11), 회전 90 -> 270
    // -----------------------------------------------------------------------
    case OptionType::Dropdown: {
        int count = (int)opt->List.size();
        int shown = opt->Expanded ? std::max(count - 1, 0) : 0;
        const float h = 40.0f + shown * 26.0f;

        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        // bkg: 크기 (width-20, h-9), 위치 (10,4)
        ImVec2 bkgMin(origin.x + 10.0f, origin.y + 4.0f);
        ImVec2 bkgMax(origin.x + width - 10.0f, origin.y + h - 5.0f);

        // 행 전체 호버 판정. InvisibleButton 을 쓰면 아래의 헤더/항목 버튼보다
        // 먼저 등록돼 클릭을 가로채므로, 히트테스트만 직접 한다.
        bool rowHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                        ImGui::IsMouseHoveringRect(bb.Min, bb.Max);

        // 원본: 호버시 Light 0.034 -> 0.0875
        float t = Tween01(ImGui::GetID("##ddh"), rowHover);
        dl->AddRectFilled(bkgMin, bkgMax,
                          Mix(Light(g_palette.Main, 0.034f), Light(g_palette.Main, 0.0875f), t), 6.0f);
        // button: (1,-2),(1,-2) @ (1,1)
        dl->AddRectFilled(ImVec2(bkgMin.x + 1, bkgMin.y + 1), ImVec2(bkgMax.x - 1, bkgMax.y - 1),
                          g_palette.Main, 6.0f);

        // 헤더 클릭 영역 (상단 29px = title 높이)
        ImVec2 hdrMin(bkgMin.x + 1, bkgMin.y + 1);
        ImVec2 hdrMax(bkgMax.x - 1, bkgMin.y + 30.0f);
        ImGui::SetCursorScreenPos(hdrMin);
        ImGui::InvisibleButton("##ddh2", ImVec2(hdrMax.x - hdrMin.x, hdrMax.y - hdrMin.y));
        bool headerClicked = ImGui::IsItemClicked();
        Tip(opt->Tooltip.c_str());

        // title: button(=bkg+1) 안의 Size (1,0),(0,29) 라벨. 공백 9칸 들여쓰기.
        const float ddIndent = HairIndent(g_fontTitle, 13.0f, kDropdownHair, kDropdownTextFallback);
        const float btnW = (bkgMax.x - 1.0f) - (bkgMin.x + 1.0f);
        std::string title = opt->Name + " - " + opt->Selected;
        TextIn(dl, g_fontTitle, 13.0f, ImVec2(bkgMin.x + 1.0f + ddIndent, bkgMin.y + 1.0f),
               ImVec2(btnW - ddIndent, 29.0f), XAlign::Left, dimText,
               Truncate(g_fontTitle, 13.0f, title, btnW - ddIndent).c_str());

        // arrow 4x8, 회전 90(접힘=아래) / 270(펼침=위)
        // Arrow(w,h) 의 w 는 회전 전 삼각형이 뾰족한 방향의 길이다.
        float rot = opt->Expanded ? 270.0f : 90.0f;
        // button 안의 (1,-17),(0,11) + 크기 4x8 -> 중심 = bkg 우측에서 16px, 상단에서 16px
        Arrow(dl, ImVec2(bkgMax.x - 16.0f, bkgMin.y + 16.0f), 4.0f, 8.0f, rot,
              IM_COL32(140, 140, 140, 255));

        if (opt->Expanded) {
            int ind = 0;
            for (int i = 0; i < count; i++) {
                if (opt->List[i] == opt->Selected) continue;   // 원본은 선택값 제외
                // dropdownchildren @ button 내 (0,27) -> 절대 y = origin + 4 + 1 + 27
                ImVec2 oMin(bkgMin.x + 1, origin.y + 32.0f + ind * 26.0f);
                ImVec2 oMax(bkgMax.x - 1, oMin.y + 26.0f);

                ImGui::SetCursorScreenPos(oMin);
                ImGui::PushID(i);
                ImGui::InvisibleButton("##opt", ImVec2(oMax.x - oMin.x, 26.0f));
                bool oh = ImGui::IsItemHovered();
                bool oc = ImGui::IsItemClicked();
                ImGui::PopID();

                // 원본: 호버시 Light(Main, 0.02). 항목엔 UICorner 가 없다.
                dl->AddRectFilled(oMin, oMax, oh ? Light(g_palette.Main, 0.02f) : g_palette.Main);
                TextIn(dl, g_fontTitle, 13.0f, ImVec2(oMin.x + ddIndent, oMin.y),
                       ImVec2(btnW - ddIndent, 26.0f), XAlign::Left, dimText,
                       Truncate(g_fontTitle, 13.0f, opt->List[i], btnW - ddIndent).c_str());

                if (oc) {
                    opt->Selected = opt->List[i];
                    opt->Expanded = false;      // SetValue 시 접힘
                    FireOption(opt, true);
                }
                ind++;
            }
        }

        if (headerClicked) opt->Expanded = !opt->Expanded;
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // TextBox - 원본 1651
    //   높이 58, title 12px @ (10,3) 색 = uipallet.Text
    //   bkg (width-20, 29) @ (10,23), corner 4, Light(Main,0.02)
    //   box 텍스트 12px, Dark(Text,0.16), placeholder Dark(Text,0.31)
    // -----------------------------------------------------------------------
    case OptionType::TextBox: {
        const float h = 58.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        // title: Size (1,-10),(0,20) @ (10,3), TextSize 12, 색은 uipallet.Text 그대로
        TextIn(dl, g_fontBox, 12.0f, ImVec2(origin.x + 10.0f, origin.y + 3.0f),
               ImVec2(width - 10.0f, 20.0f), XAlign::Left, g_palette.Text, opt->Name.c_str());

        ImVec2 boxMin(origin.x + 10.0f, origin.y + 23.0f);
        ImVec2 boxMax(origin.x + width - 10.0f, boxMin.y + 29.0f);
        dl->AddRectFilled(boxMin, boxMax, Light(g_palette.Main, 0.02f), 4.0f);

        // box: Size (1,-8),(1,0) @ (8,0), TextSize 12
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", opt->Text.c_str());
        if (g_fontBox) ImGui::PushFont(g_fontBox);
        ImGui::SetCursorScreenPos(ImVec2(boxMin.x + 8.0f,
                                         boxMin.y + (29.0f - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(boxMax.x - boxMin.x - 16.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, dimText);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Dark(g_palette.Text, 0.31f));
        bool textChanged =
            ImGui::InputTextWithHint("##tb", opt->Placeholder.c_str(), buf, sizeof(buf));
        if (textChanged) {
            opt->Text = buf;
            FireOption(opt, false);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) FireOption(opt, true);
        ImGui::PopStyleColor(3);
        if (g_fontBox) ImGui::PopFont();
        Tip(opt->Tooltip.c_str());
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // ColorSlider - 원본 540
    //   높이 50, 트랙 @ (10,39), preview 12x12 @ (width-22,10)
    //   rainbow 12x12 @ (width-42,10), expand 17x13
    //   확장시 Saturation/Vibrance/Opacity 각 50px, 트랙 @ (10,37)
    // -----------------------------------------------------------------------
    case OptionType::ColorSlider: {
        const float h = 50.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImVec2 trackMin(origin.x + 10.0f, origin.y + 39.0f);
        float  trackW = width - 20.0f;

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 20.0f));
        ImGui::InvisibleButton("##cs", ImVec2(width, h - 20.0f));
        bool held = ImGui::IsItemActive();
        bool trackHover = ImGui::IsItemHovered();
        Tip(opt->Tooltip.c_str());
        // 노브 확대는 행 전체 호버 기준 (slider.MouseEnter)
        bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                       ImGui::IsMouseHoveringRect(bb.Min, bb.Max);

        // 원본: 트랙 더블클릭 -> 무지개 토글
        if (trackHover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            opt->Color.Rainbow = !opt->Color.Rainbow;
            FireOption(opt, true);
        } else if (held) {
            float p = ImClamp((ImGui::GetIO().MousePos.x - trackMin.x) / trackW, 0.0f, 1.0f);
            opt->Color.Hue = p;
            FireOption(opt, false);
        }
        if (opt->InteractionActive && !held) FireOption(opt, true);
        opt->InteractionActive = held;

        // title: Size (60,30) @ (10,2), TextSize 11
        TextIn(dl, g_fontSmall, 11.0f, ImVec2(origin.x + 10.0f, origin.y + 2.0f),
               ImVec2(60.0f, 30.0f), XAlign::Left, dimText, opt->Name.c_str());
        float titleW = CalcTextF(g_fontSmall, 11.0f, opt->Name.c_str()).x;

        // expandicon.png 9x5 @ title 폭 + 11
        ImVec2 exMin(origin.x + titleW + 11.0f, origin.y + 7.0f);
        ImGui::SetCursorScreenPos(exMin);
        ImGui::InvisibleButton("##ex", ImVec2(17, 13));
        bool exHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) opt->Expanded = !opt->Expanded;
        // expandicon.png 9x5 @ (4,4), 원본 Rotation 은 펼침 180 / 접힘 0
        ImU32 exCol = exHover ? Dark(g_palette.Text, 0.16f) : Dark(g_palette.Text, 0.43f);
        if (!DrawAssetRotated(dl, Asset::ExpandIcon, ImVec2(exMin.x + 8.5f, exMin.y + 6.5f),
                              9.0f, 5.0f, opt->Expanded ? 180.0f : 0.0f, exCol))
            Arrow(dl, ImVec2(exMin.x + 8.0f, exMin.y + 6.0f), 5.0f, 9.0f,
                  opt->Expanded ? 270.0f : 90.0f, exCol);

        // rainbow 12x12 @ (width-42, 10)
        ImVec2 rbMin(origin.x + width - 42.0f, origin.y + 10.0f);
        ImGui::SetCursorScreenPos(rbMin);
        ImGui::InvisibleButton("##rb", ImVec2(12, 12));
        if (ImGui::IsItemClicked()) {
            opt->Color.Rainbow = !opt->Color.Rainbow;
            FireOption(opt, true);
        }
        // 원본은 0.1초 간격으로 순차 점등
        float lit = TweenVal(ImGui::GetID("##rblit"), opt->Color.Rainbow ? 1.0f : 0.0f, 0.3f, 1.0f);
        RainbowStrip(dl, rbMin, 12.0f, 12.0f, lit, Light(g_palette.Main, 0.37f));

        // preview 12x12 @ (width-22, 10)
        ImVec2 pvMin(origin.x + width - 22.0f, origin.y + 10.0f);
        ImGui::SetCursorScreenPos(pvMin);
        ImGui::InvisibleButton("##pv", ImVec2(12, 12));
        if (ImGui::IsItemClicked()) {
            opt->BoxEditing = true;
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(opt->Color.Hue, opt->Color.Sat, opt->Color.Value, r, g, b);
            std::snprintf(opt->EditBuffer, sizeof(opt->EditBuffer), "%d, %d, %d",
                          (int)std::round(r * 255), (int)std::round(g * 255), (int)std::round(b * 255));
        }

        if (opt->BoxEditing) {
            // valuebox: Size (60,15) @ (1,-69),(0,9)
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 69.0f, origin.y + 9.0f));
            ImGui::SetNextItemWidth(60.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, dimText);
            if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rgb", opt->EditBuffer, sizeof(opt->EditBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                int rgb[3] = {0, 0, 0}, n = 0;
                for (const char* p = opt->EditBuffer; *p && n < 3; ) {
                    while (*p == ' ' || *p == ',') p++;
                    if (*p < '0' || *p > '9') break;
                    int acc = 0;
                    while (*p >= '0' && *p <= '9') {
                        if (acc <= 255) acc = std::min(acc * 10 + (*p - '0'), 256);
                        p++;
                    }
                    rgb[n++] = acc;
                }
                if (n == 3) {
                    for (int& channel : rgb) channel = ImClamp(channel, 0, 255);
                    float hh, ss, vv;
                    ImGui::ColorConvertRGBtoHSV(rgb[0]/255.0f, rgb[1]/255.0f, rgb[2]/255.0f, hh, ss, vv);
                    opt->Color.Hue = hh; opt->Color.Sat = ss; opt->Color.Value = vv;
                    opt->Color.Rainbow = false;
                    FireOption(opt, true);
                }
                opt->BoxEditing = false;
            } else if (ImGui::IsItemDeactivated()) {
                opt->BoxEditing = false;
            }
            ImGui::PopStyleColor(2);
            if (g_fontSmall) ImGui::PopFont();
        } else {
            ColorPreview(dl, ImVec2(pvMin.x + 6.0f, pvMin.y + 6.0f), 12.0f,
                         HSV(opt->Color.Hue, opt->Color.Sat, opt->Color.Value), opt->Color.Opacity);
        }

        // 색조 그라디언트 트랙 - 원본은 i = 0, 0.1, ... 1 의 키포인트 11 개짜리
        // UIGradient 이므로 그 사이를 RGB 로 선형 보간한 10 구간으로 그린다.
        for (int i = 0; i < 10; i++) {
            float x0 = trackMin.x + trackW * (i / 10.0f);
            float x1 = trackMin.x + trackW * ((i + 1) / 10.0f);
            ImU32 c0 = HSV(i / 10.0f, 1, 1), c1 = HSV((i + 1) / 10.0f, 1, 1);
            dl->AddRectFilledMultiColor(ImVec2(x0, trackMin.y), ImVec2(x1, trackMin.y + 2.0f),
                                        c0, c1, c1, c0);
        }
        float hf = ImClamp(opt->Color.Hue, 0.04f, 0.96f);
        float ks = TweenVal(ImGui::GetID("##csk"), (hovered || held) ? 16.0f : 14.0f,
                            g_palette.Tween, 2.0f);
        ImVec2 kc(trackMin.x + trackW * hf, trackMin.y + 1.0f);
        dl->AddRectFilled(ImVec2(kc.x - 12.0f, kc.y - 2.0f), ImVec2(kc.x + 12.0f, kc.y + 2.0f), rowBg);
        dl->AddCircleFilled(kc, ks * 0.5f, g_palette.Text, 24);
        y += h;

        // 확장 서브 슬라이더 3개 (각 50px, 트랙 @ y+37)
        if (opt->Expanded) {
            struct Sub { const char* name; float* val; ImU32 c0, c1; };
            ImU32 satA = HSV(0, 0, opt->Color.Value), satB = HSV(opt->Color.Hue, 1, opt->Color.Value);
            ImU32 vibA = HSV(0, 0, 0), vibB = HSV(opt->Color.Hue, opt->Color.Sat, 1);
            ImU32 opA = Dark(g_palette.Main, 0.02f);
            ImU32 opB = HSV(opt->Color.Hue, opt->Color.Sat, opt->Color.Value);
            Sub subs[3] = {
                { "Saturation", &opt->Color.Sat,     satA, satB },
                { "Vibrance",   &opt->Color.Value,   vibA, vibB },
                { "Opacity",    &opt->Color.Opacity, opA,  opB  },
            };
            for (int i = 0; i < 3; i++) {
                ImVec2 so(wp.x, wp.y + y);
                dl->AddRectFilled(so, ImVec2(so.x + width, so.y + 50.0f), rowBg);

                ImVec2 stMin(so.x + 10.0f, so.y + 37.0f);
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(ImVec2(so.x, so.y + 20.0f));
                ImGui::InvisibleButton("##sub", ImVec2(width, 30.0f));
                bool sHeld = ImGui::IsItemActive();
                // 노브 확대는 서브 슬라이더 행 전체(50px) 호버 기준
                bool sh = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                          ImGui::IsMouseHoveringRect(so, ImVec2(so.x + width, so.y + 50.0f));
                if (sHeld) {
                    *subs[i].val = ImClamp((ImGui::GetIO().MousePos.x - stMin.x) / trackW, 0.0f, 1.0f);
                    FireOption(opt, false);
                }
                if (ImGui::IsItemDeactivated()) FireOption(opt, true);
                // title: Size (60,30) @ (10,2), TextSize 11
                TextIn(dl, g_fontSmall, 11.0f, ImVec2(so.x + 10.0f, so.y + 2.0f),
                       ImVec2(60.0f, 30.0f), XAlign::Left, dimText, subs[i].name);
                dl->AddRectFilledMultiColor(stMin, ImVec2(stMin.x + trackW, stMin.y + 2.0f),
                                            subs[i].c0, subs[i].c1, subs[i].c1, subs[i].c0);
                float sf = ImClamp(*subs[i].val, 0.04f, 0.96f);
                float sks = TweenVal(ImGui::GetID("##subk"), (sh || sHeld) ? 16.0f : 14.0f,
                                     g_palette.Tween, 2.0f);
                ImVec2 skc(stMin.x + trackW * sf, stMin.y + 1.0f);
                dl->AddRectFilled(ImVec2(skc.x - 12.0f, skc.y - 2.0f),
                                  ImVec2(skc.x + 12.0f, skc.y + 2.0f), rowBg);
                dl->AddCircleFilled(skc, sks * 0.5f, g_palette.Text, 24);
                ImGui::PopID();
                y += 50.0f;
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // Button - 원본 499
    //   높이 31, bkg 200x27 @ (10,2), label (1,-4),(1,-4) @ (2,2)
    //   호버 Light 0.05 -> 0.0875, TextSize 14
    // -----------------------------------------------------------------------
    case OptionType::Button: {
        const float h = 31.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImGui::SetCursorScreenPos(bb.Min);
        ImGui::InvisibleButton("##bt", ImVec2(width, h));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) FireOption(opt, true);
        Tip(opt->Tooltip.c_str());

        ImVec2 pMin(origin.x + 10.0f, origin.y + 2.0f);
        ImVec2 pMax(pMin.x + width - 20.0f, pMin.y + 27.0f);
        float t = Tween01(ImGui::GetID("##bth"), hovered);
        dl->AddRectFilled(pMin, pMax,
                          Mix(Light(g_palette.Main, 0.05f), Light(g_palette.Main, 0.0875f), t), 5.0f);
        dl->AddRectFilled(ImVec2(pMin.x + 2, pMin.y + 2), ImVec2(pMax.x - 2, pMax.y - 2),
                          g_palette.Main, 4.0f);

        // label: Size (1,-4),(1,-4) @ (2,2), TextSize 14, 가로/세로 중앙
        TextIn(dl, Font14(), 14.0f, ImVec2(pMin.x + 2.0f, pMin.y + 2.0f),
               ImVec2((pMax.x - pMin.x) - 4.0f, (pMax.y - pMin.y) - 4.0f),
               XAlign::Center, dimText, opt->Name.c_str());
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // TextList - 원본 1731
    //   높이 50, bkg (width-20, h-9) @ (10,4) corner 4
    //   icon 14x12 @ (10,14), title @ (35,6) 15px, amount 우측, items @ (35,21) 11px
    //   창 220 x (85 + 개수*35), 창 위치 = 행 x + 220
    // -----------------------------------------------------------------------
    case OptionType::TextList: {
        const float h = 50.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImVec2 bkgMin(origin.x + 10.0f, origin.y + 4.0f);
        ImVec2 bkgMax(origin.x + width - 10.0f, origin.y + h - 5.0f);

        ImGui::SetCursorScreenPos(bkgMin);
        ImGui::InvisibleButton("##tl", ImVec2(bkgMax.x - bkgMin.x, bkgMax.y - bkgMin.y));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) opt->WindowOpen = !opt->WindowOpen;
        Tip(opt->Tooltip.c_str());

        // 원본: 창 열림 = 강조색, 호버 = Light 0.37, 기본 = Light 0.034
        ImU32 border = opt->WindowOpen ? g_palette.Accent()
                                       : Mix(Light(g_palette.Main, 0.034f),
                                             Light(g_palette.Main, 0.37f),
                                             Tween01(ImGui::GetID("##tlh"), hovered));
        dl->AddRectFilled(bkgMin, bkgMax, border, 4.0f);
        // button: (1,-2),(1,-2) @ (1,1) - 아래 자식들은 전부 이 기준점을 쓴다.
        ImVec2 btn(bkgMin.x + 1.0f, bkgMin.y + 1.0f);
        float  btnW = (bkgMax.x - 1.0f) - btn.x;
        dl->AddRectFilled(btn, ImVec2(bkgMax.x - 1, bkgMax.y - 1), g_palette.Main, 4.0f);

        // buttonicon 14x12 @ button 내 (10,14)
        if (!DrawAssetSized(dl, Asset::AllowedIcon, ImVec2(btn.x + 10.0f, btn.y + 14.0f),
                            14.0f, 12.0f, IM_COL32(255, 255, 255, 255))) {
            ImVec2 ic(btn.x + 17.0f, btn.y + 20.0f);
            dl->AddLine(ImVec2(ic.x - 4.0f, ic.y), ImVec2(ic.x - 1.0f, ic.y + 3.0f),
                        Light(g_palette.Main, 0.6f), 1.6f);
            dl->AddLine(ImVec2(ic.x - 1.0f, ic.y + 3.0f), ImVec2(ic.x + 4.0f, ic.y - 3.0f),
                        Light(g_palette.Main, 0.6f), 1.6f);
        }

        // buttontitle: Size (1,-35),(0,15) @ (35,6), TextSize 15, TextTruncate.AtEnd
        TextIn(dl, g_fontItem, 15.0f, ImVec2(btn.x + 35.0f, btn.y + 6.0f),
               ImVec2(btnW - 35.0f, 15.0f), XAlign::Left, dimText,
               Truncate(g_fontItem, 15.0f, opt->Name, btnW - 35.0f).c_str());

        // amount: Size (1,-13),(0,15) @ (0,6), 오른쪽 정렬
        std::string cnt = std::to_string(opt->ListEntries.size());
        TextIn(dl, g_fontItem, 15.0f, ImVec2(btn.x, btn.y + 6.0f),
               ImVec2(btnW - 13.0f, 15.0f), XAlign::Right, dimText, cnt.c_str());

        // items: Size (1,-35),(0,15) @ (35,21), TextSize 11, 색 Dark(Text,0.43)
        std::string items = "None";
        for (size_t i = 0; i < opt->ListEnabled.size(); i++)
            items = (i == 0) ? opt->ListEnabled[i] : items + ", " + opt->ListEnabled[i];
        TextIn(dl, g_fontSmall, 11.0f, ImVec2(btn.x + 35.0f, btn.y + 21.0f),
               ImVec2(btnW - 35.0f, 15.0f), XAlign::Left, Dark(g_palette.Text, 0.43f),
               Truncate(g_fontSmall, 11.0f, items, btnW - 35.0f).c_str());

        // 창은 카테고리 창들보다 위에 그려야 하므로 Render() 마지막에 그린다.
        opt->RowScreenPos = origin;
        opt->RowSeen = true;
        y += h;
        break;
    }

    // -----------------------------------------------------------------------
    // Targets - 원본 1336
    //   높이 50, title 'Target:' @ (5,6), items @ (5,21)
    //   창 220x145
    // -----------------------------------------------------------------------
    case OptionType::Targets: {
        const float h = 50.0f;
        ImRect bb(origin, ImVec2(origin.x + width, origin.y + h));
        dl->AddRectFilled(bb.Min, bb.Max, rowBg);

        ImVec2 bkgMin(origin.x + 10.0f, origin.y + 4.0f);
        ImVec2 bkgMax(origin.x + width - 10.0f, origin.y + h - 5.0f);

        ImGui::SetCursorScreenPos(bkgMin);
        ImGui::InvisibleButton("##tgt", ImVec2(bkgMax.x - bkgMin.x, bkgMax.y - bkgMin.y));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) opt->WindowOpen = !opt->WindowOpen;
        Tip(opt->Tooltip.c_str());

        ImU32 border = opt->WindowOpen ? g_palette.Accent()
                                       : Mix(Light(g_palette.Main, 0.034f),
                                             Light(g_palette.Main, 0.37f),
                                             Tween01(ImGui::GetID("##tgth"), hovered));
        dl->AddRectFilled(bkgMin, bkgMax, border, 4.0f);
        // button: (1,-2),(1,-2) @ (1,1)
        ImVec2 btn(bkgMin.x + 1.0f, bkgMin.y + 1.0f);
        float  btnW = (bkgMax.x - 1.0f) - btn.x;
        dl->AddRectFilled(btn, ImVec2(bkgMax.x - 1, bkgMax.y - 1), g_palette.Main, 4.0f);

        // buttontitle: Size (1,-5),(0,15) @ (5,6), TextSize 15
        TextIn(dl, g_fontItem, 15.0f, ImVec2(btn.x + 5.0f, btn.y + 6.0f),
               ImVec2(btnW - 5.0f, 15.0f), XAlign::Left, dimText, "Target:");

        // tool: Frame 65x12 @ (52,8) + UIListLayout(Horizontal, Padding 6)
        // 원본: targetplayers2.png 11x12, targetnpc2.png 9x12, 색 uipallet.Text
        float tx = btn.x + 52.0f;
        const float ty = btn.y + 8.0f;
        if (opt->Players) {
            if (!DrawAssetSized(dl, Asset::TargetPlayers2, ImVec2(tx, ty),
                                11.0f, 12.0f, g_palette.Text))
                CatIcon(dl, 8, ImVec2(tx + 5.5f, ty + 6.0f), 5.0f, g_palette.Text);
            tx += 11.0f + 6.0f;
        }
        if (opt->NPCs) {
            if (!DrawAssetSized(dl, Asset::TargetNpc2, ImVec2(tx, ty),
                                9.0f, 12.0f, g_palette.Text))
                CatIcon(dl, 5, ImVec2(tx + 4.5f, ty + 6.0f), 5.0f, g_palette.Text);
        }

        // items: Size (1,-5),(0,15) @ (5,21), TextSize 11, 색 Dark(Text,0.16)
        std::string txt = "none";
        if (opt->Invisible) txt = "invisible";
        if (opt->Walls) txt = (txt == "none") ? "behind walls" : txt + ", behind walls";
        std::string items = "Ignore " + txt;
        TextIn(dl, g_fontSmall, 11.0f, ImVec2(btn.x + 5.0f, btn.y + 21.0f),
               ImVec2(btnW - 5.0f, 15.0f), XAlign::Left, dimText, items.c_str());

        opt->RowScreenPos = origin;
        opt->RowSeen = true;
        y += h;
        break;
    }
    }

    ImGui::PopID();
}

// ===========================================================================
// TextList / Targets 의 별도 창
//
// 원본에서 이 창들은 clickgui 에 직접 붙고, 행의 절대 좌표를 따라다닌다:
//     window.Position = (row.X + 220, row.Y)
// 여기서도 행을 그릴 때 기록해 둔 좌표를 그대로 쓴다.
// ===========================================================================

namespace {

// 창 공통 껍데기. 제목/아이콘/닫기까지 그리고 본문 그리기 준비를 마친다.
// 반환값: Begin 이 성공했는지. true 면 호출부가 반드시 End() 해야 한다.
bool BeginPopupWindow(Option* opt, const char* idTag, float w, float h,
                      Asset icon, float iw, float ih, float ix, float iy,
                      const char* title, ImVec2* outPos) {
    char id[64];
    std::snprintf(id, sizeof(id), "##%s%p", idTag, (void*)opt);

    ImGui::SetNextWindowPos(ImVec2(opt->RowScreenPos.x + kWinW, opt->RowScreenPos.y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (!opt->WindowWasOpen) ImGui::SetNextWindowFocus();   // 방금 열렸으면 맨 앞으로
    opt->WindowWasOpen = true;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);   // addCorner 기본 5
    ImGui::PushStyleColor(ImGuiCol_WindowBg, g_palette.Main);

    bool open = ImGui::Begin(id, nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings);
    if (!open) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    *outPos = wp;
    Blur(wp, ImVec2(wp.x + w, wp.y + h));   // addBlur(window)

    // icon - 원본은 ImageColor3 를 지정하지 않으므로 색 보정 없이 그린다.
    DrawAssetSized(dl, icon, ImVec2(wp.x + ix, wp.y + iy), iw, ih,
                   IM_COL32(255, 255, 255, 255));
    // title: Size (1,-36),(0,20) @ (36,11), TextSize 13, 색 uipallet.Text
    TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + 36.0f, wp.y + 11.0f),
           ImVec2(w - 36.0f, 20.0f), XAlign::Left, g_palette.Text, title);

    if (CloseButton(dl, wp, w)) opt->WindowOpen = false;
    return true;
}

void EndPopupWindow() {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace

// ---------------------------------------------------------------------------
// TextList 창  (원본 components/TextList.lua)
//   220 x (85 + 개수*35)
//   icon 19x16 @ (10,13), addbkg 200x31 @ (10,45)
//   항목 200x32 @ (10, 47 + i*35)   (i 는 1 부터)
// ---------------------------------------------------------------------------
void VapeGui::RenderTextListWindow(Option* opt) {
    const int   n = (int)opt->ListEntries.size();
    const float w = kWinW;
    const float h = 85.0f + n * 35.0f;

    ImVec2 wp;
    if (!BeginPopupWindow(opt, "tlwin", w, h, Asset::AllowedTab, 19.0f, 16.0f, 10.0f, 13.0f,
                          opt->Name.c_str(), &wp))
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 dimText = Dark(g_palette.Text, 0.16f);

    // --- addbkg 200x31 @ (10,45) ------------------------------------------
    ImVec2 addMin(wp.x + 10.0f, wp.y + 45.0f);
    ImVec2 addMax(addMin.x + 200.0f, addMin.y + 31.0f);

    // 원본은 addvalue(TextBox) 호버에서만 색이 바뀐다.
    bool addHover = ImGui::IsWindowHovered() &&
                    ImGui::IsMouseHoveringRect(addMin, ImVec2(addMax.x - 35.0f, addMax.y));
    float at = Tween01(ImGui::GetID("##addh"), addHover);
    dl->AddRectFilled(addMin, addMax,
                      Mix(Light(g_palette.Main, 0.02f), Light(g_palette.Main, 0.14f), at), 5.0f);
    // addbox: (1,-2),(1,-2) @ (1,1), Dark(Main, 0.02)
    dl->AddRectFilled(ImVec2(addMin.x + 1, addMin.y + 1), ImVec2(addMax.x - 1, addMax.y - 1),
                      Dark(g_palette.Main, 0.02f), 5.0f);

    auto commit = [&]() {
        std::string v = opt->AddBuffer;
        if (v.empty()) return;
        for (auto& e : opt->ListEntries) if (e == v) return;   // 원본: 중복이면 무시
        opt->ListEntries.push_back(v);
        opt->ListEnabled.push_back(v);
        opt->AddBuffer[0] = '\0';
        FireOption(opt, true);
    };

    // addvalue: Size (1,-35),(1,0) @ (10,0), TextSize 15, 흰색
    if (g_fontItem) ImGui::PushFont(g_fontItem);
    ImGui::SetCursorScreenPos(ImVec2(addMin.x + 10.0f,
                                     addMin.y + (31.0f - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(200.0f - 35.0f - 10.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Dark(g_palette.Text, 0.31f));
    if (ImGui::InputTextWithHint("##add", opt->Placeholder.c_str(), opt->AddBuffer,
                                 sizeof(opt->AddBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
        commit();
    ImGui::PopStyleColor(3);
    if (g_fontItem) ImGui::PopFont();

    // addbutton: 16x16 @ (1,-26),(0,8), add.png, ImageTransparency 0.3 -> 0
    ImVec2 abMin(addMax.x - 26.0f, addMin.y + 8.0f);
    ImGui::SetCursorScreenPos(abMin);
    ImGui::InvisibleButton("##addbtn", ImVec2(16, 16));
    bool abHover = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) commit();
    {
        ImVec4 c = V4(opt->ListColor);
        c.w = abHover ? 1.0f : 0.7f;
        if (!DrawAssetSized(dl, Asset::Add, abMin, 16.0f, 16.0f, U32(c)))
            Plus(dl, ImVec2(abMin.x + 8.0f, abMin.y + 8.0f), 10.0f, U32(c));
    }

    // --- 항목들 ------------------------------------------------------------
    for (int i = 0; i < n; i++) {
        const std::string v = opt->ListEntries[(size_t)i];
        bool enabled = false;
        for (auto& e : opt->ListEnabled) if (e == v) { enabled = true; break; }

        // 원본 인덱스는 1 부터이므로 y = 47 + (i+1)*35
        ImVec2 oMin(wp.x + 10.0f, wp.y + 47.0f + (i + 1) * 35.0f);
        ImVec2 oMax(oMin.x + 200.0f, oMin.y + 32.0f);

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(oMin);
        ImGui::InvisibleButton("##ent", ImVec2(200, 32));
        bool oHover = ImGui::IsItemHovered();
        bool oClick = ImGui::IsItemClicked();
        ImVec2 cMin(oMax.x - 26.0f, oMin.y + 8.0f);
        const bool cHover = oHover &&
                            ImGui::IsMouseHoveringRect(cMin, ImVec2(cMin.x + 16.0f, cMin.y + 16.0f));

        dl->AddRectFilled(oMin, oMax, Light(g_palette.Main, 0.02f), 5.0f);
        // objectbkg: (1,-2),(1,-2) @ (1,1), 호버시에만 보인다.
        if (oHover)
            dl->AddRectFilled(ImVec2(oMin.x + 1, oMin.y + 1), ImVec2(oMax.x - 1, oMax.y - 1),
                              g_palette.Main, 5.0f);

        // objectdot 10x11 @ (10,12) + objectdotin 8x9 @ (1,1)
        ImVec2 dotMin(oMin.x + 10.0f, oMin.y + 12.0f);
        dl->AddRectFilled(dotMin, ImVec2(dotMin.x + 10.0f, dotMin.y + 11.0f),
                          enabled ? opt->ListColor : Light(g_palette.Main, 0.37f), 5.0f);
        dl->AddRectFilled(ImVec2(dotMin.x + 1, dotMin.y + 1),
                          ImVec2(dotMin.x + 9.0f, dotMin.y + 10.0f),
                          enabled ? opt->ListColor : Light(g_palette.Main, 0.02f), 4.0f);

        // objecttitle: Size (1,-30),(1,0) @ (30,0), TextSize 15
        TextIn(dl, g_fontItem, 15.0f, ImVec2(oMin.x + 30.0f, oMin.y), ImVec2(170.0f, 32.0f),
               XAlign::Left, dimText, Truncate(g_fontItem, 15.0f, v, 170.0f).c_str());

        // close: 16x16 @ (1,-26),(0,8), closemini.png
        bool cClick = oClick && cHover;
        if (cHover)   // BackgroundTransparency 1 -> 0.6
            dl->AddRectFilled(cMin, ImVec2(cMin.x + 16.0f, cMin.y + 16.0f),
                              IM_COL32(255, 255, 255, 102), 8.0f);
        {
            ImVec4 c = V4(Light(g_palette.Text, 0.2f));
            c.w = cHover ? 0.7f : 0.5f;   // ImageTransparency 0.5 -> 0.3
            Cross(dl, ImVec2(cMin.x + 8.0f, cMin.y + 8.0f), 16.0f, U32(c));
        }
        ImGui::PopID();

        if (cClick) {
            // 원본 ChangeValue(v): List 와 ListEnabled 양쪽에서 제거
            opt->ListEntries.erase(opt->ListEntries.begin() + i);
            for (size_t k = 0; k < opt->ListEnabled.size(); k++)
                if (opt->ListEnabled[k] == v) { opt->ListEnabled.erase(opt->ListEnabled.begin() + k); break; }
            FireOption(opt, true);
            break;
        }
        if (oClick && !cHover) {
            bool removed = false;
            for (size_t k = 0; k < opt->ListEnabled.size(); k++)
                if (opt->ListEnabled[k] == v) {
                    opt->ListEnabled.erase(opt->ListEnabled.begin() + k);
                    removed = true;
                    break;
                }
            if (!removed) opt->ListEnabled.push_back(v);
            FireOption(opt, true);
        }
    }

    EndPopupWindow();
}

// ---------------------------------------------------------------------------
// Targets 창  (원본 components/Targets.lua)
//   220x145, icon 18x12 @ (10,15)
//   TargetsButton 98x31 @ (11,45) / (112,45)
//   Toggle @ (0,81) / (0,111)
// ---------------------------------------------------------------------------
void VapeGui::RenderTargetsWindow(Option* opt) {
    const float w = kWinW, h = 145.0f;

    ImVec2 wp;
    if (!BeginPopupWindow(opt, "tgtwin", w, h, Asset::TargetsTab, 18.0f, 12.0f, 10.0f, 15.0f,
                          "Target settings", &wp))
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // components.TargetsButton - 98x31, bkg (1,-2),(1,-2) @ (1,1)
    auto targetsButton = [&](const char* id, ImVec2 pos, bool& value,
                             Asset icon, float iw, float ih, const char* tip) {
        ImVec2 bMin(wp.x + pos.x, wp.y + pos.y);
        ImVec2 bMax(bMin.x + 98.0f, bMin.y + 31.0f);
        ImGui::PushID(id);
        ImGui::SetCursorScreenPos(bMin);
        ImGui::InvisibleButton("##tb", ImVec2(98, 31));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            value = !value;
            FireOption(opt, true);
        }
        Tip(tip);
        ImGui::PopID();

        dl->AddRectFilled(bMin, bMax, Light(g_palette.Main, 0.05f), 5.0f);
        // 켜짐 = 강조색 / 꺼짐+호버 = HSV(h, s, v - 0.25) / 그 외 = Main
        ImU32 inner = value ? g_palette.Accent()
                            : (hovered ? HSV(g_palette.Hue, g_palette.Sat,
                                             ImMax(g_palette.Value - 0.25f, 0.0f))
                                       : g_palette.Main);
        dl->AddRectFilled(ImVec2(bMin.x + 1, bMin.y + 1), ImVec2(bMax.x - 1, bMax.y - 1),
                          inner, 5.0f);

        ImU32 ic = (value || hovered) ? IM_COL32(255, 255, 255, 255)
                                      : Light(g_palette.Main, 0.37f);
        ImVec2 ip((bMin.x + bMax.x - iw) * 0.5f, (bMin.y + bMax.y - ih) * 0.5f);
        if (!DrawAssetSized(dl, icon, ip, iw, ih, ic))
            dl->AddRectFilled(ip, ImVec2(ip.x + iw, ip.y + ih), ic, 2.0f);
    };

    targetsButton("players", ImVec2(11, 45), opt->Players,
                  Asset::TargetPlayers1, 15.0f, 16.0f, "Players");
    targetsButton("npcs", ImVec2(112, 45), opt->NPCs,
                  Asset::TargetNpc1, 12.0f, 16.0f, "NPCs");

    // 창 배경이 uipallet.Main 이므로 Toggle 의 행 배경도 Main 이다.
    ImGui::PushID("inv");
    if (ToggleRow(ImVec2(wp.x, wp.y + 81.0f), w, "Ignore invisible", opt->Invisible,
                  g_palette.Main, 0, nullptr))
        FireOption(opt, true);
    ImGui::PopID();
    ImGui::PushID("walls");
    if (ToggleRow(ImVec2(wp.x, wp.y + 111.0f), w, "Ignore behind walls", opt->Walls,
                  g_palette.Main, 0, nullptr))
        FireOption(opt, true);
    ImGui::PopID();

    EndPopupWindow();
}

// ===========================================================================
// 모듈 렌더링  (원본 3703)
//   modulebutton 220x40, 텍스트 앞 공백 12칸, TextSize 14
//   bind 20x21 @ (1,-36),(0,9),  dots 3x16 @ dotsbutton(25x40) 내 (4,12)
//   호버: TextColor -> Text, BackgroundColor -> Light(Main, 0.02)
//   켜짐: divider 표시, gradient 활성, dots -> RGB(50,50,50)
// ===========================================================================

void VapeGui::RenderModule(Category* cat, Module* mod, float& y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 origin(wp.x, wp.y + y);
    const float h = 40.0f;
    const float W = ImGui::GetWindowWidth();   // 스크롤바 반영된 실제 폭

    ImGui::PushID(mod);

    const std::string bindText = JoinUpper(mod->Bind);
    const bool hasBind = !mod->Bind.empty();
    float bindWidth = 20.0f;
    if (hasBind)
        bindWidth = ImClamp(CalcTextF(g_fontBox, 12.0f, bindText.c_str()).x + 10.0f,
                            20.0f, 96.0f);
    const std::string bindDisplay = hasBind
        ? Truncate(g_fontBox, 12.0f, bindText, bindWidth - 8.0f) : std::string();
    const ImVec2 bindMin(origin.x + W - 36.0f - bindWidth, origin.y + 9.0f);
    const ImVec2 bindMax(bindMin.x + bindWidth, bindMin.y + 21.0f);

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##mod", ImVec2(W, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool bindHover = hovered && ImGui::IsMouseHoveringRect(bindMin, bindMax);
    const bool dotsHover = hovered &&
                           ImGui::IsMouseHoveringRect(ImVec2(origin.x + W - 25.0f, origin.y),
                                                     ImVec2(origin.x + W, origin.y + h));
    const bool bindVisible = hasBind || hovered || mod->ChildrenVisible;
    const bool activelyBinding = BindingModule == mod;
    const bool showBindCover = activelyBinding || mod->BindCoverTimer > 0.0f;
    const float completedCoverWidth = ImMax(bindMin.x - origin.x - 4.0f, 0.0f);
    const float requiredCoverWidth = CalcTextF(
        g_fontSmall, 11.0f, mod->BindCoverText.c_str()).x + 20.0f;
    const bool fullBindCover = activelyBinding ||
        (showBindCover && completedCoverWidth < requiredCoverWidth);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (dotsHover) mod->ChildrenVisible = !mod->ChildrenVisible;
        else if (bindHover) BeginModuleBinding(mod);
        else ToggleModule(mod);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        mod->ChildrenVisible = !mod->ChildrenVisible;

    // 원본 UpdateGUI(4991): 켜진 모듈은 배경 전체가 강조색이고 글자/아이콘은
    // TextColor(hue,sat,val) 이다. 꺼진 모듈만 호버/펼침에 따라 밝아진다.
    // (MouseEnter/Leave 는 not Enabled 일 때만 색을 바꾸므로, 켜진 동안은
    //  Toggle 시점의 강조색 상태가 유지된다.)
    ImU32 bg, txt, iconCol;
    if (mod->Enabled) {
        // 무지개 모드에서 모듈 인덱스 간격은 0.025 (옵션의 0.075 와 다르다)
        bg = g_palette.Rainbow ? g_palette.AccentStep(mod->Index, 0.025f)
                               : g_palette.Accent();
        txt = iconCol = TextOnAccent(g_palette.Hue, g_palette.Sat, g_palette.Value);
    } else {
        bool lit = hovered || mod->ChildrenVisible;
        bg = lit ? Light(g_palette.Main, 0.02f) : g_palette.Main;
        txt = lit ? g_palette.Text : Dark(g_palette.Text, 0.16f);
        iconCol = Dark(g_palette.Text, 0.43f);
    }
    // 바인드 상태는 모듈의 활성 강조색과 분리한다. 켜진 모듈의 초록 배경이
    // 상태 행 뒤에 비치지 않도록 한 장의 불투명한 어두운 행으로 교체한다.
    if (showBindCover) {
        bg = Dark(g_palette.Main, 0.02f);
        txt = g_palette.Text;
        iconCol = Dark(g_palette.Text, 0.16f);
    }
    dl->AddRectFilled(origin, ImVec2(origin.x + W, origin.y + h), bg);

    // divider: (1,0),(0,1) @ (0, 1, -1), RGB(48,48,48), 투명도 0.52, 켜졌을 때만
    if (mod->Enabled && !showBindCover)
        dl->AddRectFilled(ImVec2(origin.x, origin.y + h - 1.0f),
                          ImVec2(origin.x + W, origin.y + h),
                          IM_COL32(48, 48, 48, 122));

    // 모듈 이름 - HAIR SPACE 12칸, TextSize 14
    if (!showBindCover) {
        TextIn(dl, Font14(), 14.0f,
               ImVec2(origin.x + HairIndent(Font14(), 14.0f, kModuleHair, kModuleTextFallback), origin.y),
               ImVec2(W, h), XAlign::Left, txt, mod->Name.c_str());
        if (!mod->ExtraText.empty()) {
            const float extraX = origin.x + 104.0f;
            const float extraW = ImMax(bindMin.x - extraX - 6.0f, 0.0f);
            if (extraW > 12.0f)
                TextIn(dl, g_fontSmall, 11.0f, ImVec2(extraX, origin.y),
                       ImVec2(extraW, h), XAlign::Right,
                       mod->Enabled ? txt : Dark(g_palette.Text, 0.43f),
                       Truncate(g_fontSmall, 11.0f, mod->ExtraText, extraW).c_str());
        }
    }
    Tip(mod->Tooltip.c_str());

    // bind 버튼 @ (1,-36),(0,9), AnchorPoint (1,0) - 우측 기준.
    // 바인드가 없으면 20x21, 있으면 max(글자폭 + 10, 20) x 21.
    {
        const float bw = bindWidth;

        ImVec2 bMin = bindMin;
        ImVec2 bMax = bindMax;

        if (bindVisible && !fullBindCover) {
            // 원본: BackgroundColor3 흰색, Transparency 0.92
            dl->AddRectFilled(bMin, bMax, IM_COL32(255, 255, 255, 20), 4.0f);

            // 원본: 호버하면 바인드 글자 대신 edit 아이콘을 보여준다.
            if (hasBind && !bindHover) {
                TextIn(dl, g_fontBox, 12.0f, ImVec2(bMin.x, bMin.y + 1.0f), ImVec2(bw, 21.0f),
                       XAlign::Center, mod->Enabled ? iconCol : Dark(g_palette.Text, 0.43f),
                       bindDisplay.c_str());
            } else {
                ImU32 ic = bindHover && !mod->Enabled ? Dark(g_palette.Text, 0.16f) : iconCol;
                Asset a = bindHover ? Asset::Edit : Asset::Bind;
                if (!DrawAssetSized(dl, a, ImVec2(bMin.x + bw * 0.5f - 6.0f, bMin.y + 5.0f),
                                    12.0f, 12.0f, ic)) {
                    ImVec2 bc(bMin.x + bw * 0.5f, bMin.y + 11.0f);
                    dl->AddRect(ImVec2(bc.x - 4.0f, bc.y - 4.0f),
                                ImVec2(bc.x + 4.0f, bc.y + 4.0f),
                                ic, 1.5f, 0, 1.3f);
                }
            }
        }
    }

    // 상태 문구는 모듈의 단일 배경 위에 직접 그린다. bindbkg의 투명 사선과
    // 기존 행 배경이 겹치던 이중 레이어는 사용하지 않는다.
    if (showBindCover) {
        const std::string& t = mod->BindCoverText;
        const float cw = fullBindCover ? W - 25.0f : completedCoverWidth;
        TextIn(dl, g_fontSmall, 11.0f, origin, ImVec2(cw - 10.0f, 37.0f),
               XAlign::Center, g_palette.Text, t.c_str());
    }

    // dots 3x16, dotsbutton 25x40 @ (kWinW-25, 0), dots @ (4,12)
    ImVec2 dMin(origin.x + W - 25.0f, origin.y);
    // 켜짐: 글자색과 동일 / 꺼짐: 호버 Text, 기본 Light(Main, 0.37)
    ImU32 dotsCol = mod->Enabled ? txt
                                 : (dotsHover ? g_palette.Text : Light(g_palette.Main, 0.37f));
    Dots(dl, ImVec2(dMin.x + 4.0f, dMin.y + 12.0f), dotsCol);

    y += h;

    // 옵션 패널 (modulechildren) - 배경 Dark(Main, 0.02)
    if (mod->ChildrenVisible) {
        float panelStart = y;
        // 스크롤바가 있으면 가용 폭이 220 보다 작으므로 실제 폭을 쓴다.
        float availW = ImGui::GetWindowWidth();
        const ImU32 childrenBg = Dark(g_palette.Main, 0.02f);
        for (auto& opt : mod->Options) RenderOption(opt.get(), availW, y, false, childrenBg);
        // 패널 배경을 옵션 뒤에 그리기 위해 채널을 쓰지 않고 미리 칠한다:
        // 각 옵션이 자기 배경을 칠하므로 여기서는 빈 영역만 보정.
        (void)panelStart;
    }

    ImGui::PopID();
}

// ===========================================================================
// 카테고리 창  (원본 3625)
//   220 x 41 (접힘) / 41 + 내용높이 (펼침, 최대 601)
//   icon @ (12,13), title 13px, arrow 9x4 @ arrowbutton(40x40) 내 (20,18)
//   children @ (0,37), divider @ (0,37)
// ===========================================================================

void VapeGui::RenderCategory(Category* cat) {
    if (!cat->Visible) return;

    // 내용 높이 계산
    float contentH = 0.0f;
    for (auto& m : cat->Modules) {
        contentH += 40.0f;
        if (m->ChildrenVisible) {
            for (auto& o : m->Options) {
                if (!o->Visible) continue;
                switch (o->Type) {
                case OptionType::Toggle:      contentH += 30.0f; break;
                case OptionType::Button:      contentH += 31.0f; break;
                case OptionType::TextBox:     contentH += 58.0f; break;
                case OptionType::Dropdown:
                    contentH += 40.0f + (o->Expanded ? std::max((int)o->List.size() - 1, 0) * 26.0f : 0.0f);
                    break;
                case OptionType::ColorSlider:
                    contentH += 50.0f + (o->Expanded ? 150.0f : 0.0f);
                    break;
                default:                      contentH += 50.0f; break;
                }
            }
        }
    }

    float winH = cat->Expanded ? std::min(41.0f + contentH, 601.0f) : 41.0f;

    ImGui::SetNextWindowPos(cat->Position, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kWinW, winH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);   // addCorner 기본 5
    ImGui::PushStyleColor(ImGuiCol_WindowBg, g_palette.Main);

    std::string id = cat->Name + "##cat";
    if (ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        cat->Position = wp;
        Blur(wp, ImVec2(wp.x + kWinW, wp.y + winH));

        // icon @ (12, 아이콘 폭 > 20 이면 14 아니면 13), 색 = uipallet.Text
        const CatIconDef* def = FindCatIcon(cat->Name);
        const float iconW = def ? def->w : 14.0f;
        const float iconH = def ? def->h : 14.0f;
        const float iconY = (iconW > 20.0f) ? 14.0f : 13.0f;
        if (!DrawCatIconTex(dl, cat->Name,
                            ImVec2(wp.x + 12.0f + iconW * 0.5f, wp.y + iconY + iconH * 0.5f),
                            g_palette.Text))
            CatIcon(dl, cat->IconIndex, ImVec2(wp.x + 12.0f + 8.0f, wp.y + iconY + 8.0f), 8.0f,
                    g_palette.Text);

        // title: Size (1, -(iconW > 18 and 40 or 33)),(0,41) @ (그 절대값, 0)
        // -> x 는 40 또는 33, 세로는 41px 안에서 중앙
        const float titleX = (iconW > 18.0f) ? 40.0f : 33.0f;
        TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + titleX, wp.y),
               ImVec2(kWinW - titleX, 41.0f), XAlign::Left, g_palette.Text, cat->Name.c_str());

        // arrowbutton 40x40 @ (kWinW-40, 0), arrow 9x4 @ (20,18), 회전 180(접힘)/0(펼침)
        ImGui::SetCursorScreenPos(ImVec2(wp.x + kWinW - 40.0f, wp.y));
        ImGui::InvisibleButton("##arw", ImVec2(40, 40),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool aHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
            ImGui::IsItemClicked(ImGuiMouseButton_Right)) cat->Expand();
        // 원본: 호버 RGB(220,220,220), 기본 RGB(140,140,140)
        ImU32 aCol = aHover ? IM_COL32(220, 220, 220, 255) : IM_COL32(140, 140, 140, 255);
        // expandup.png 9x4 @ arrowbutton 내 (20,18). 원본 Rotation 은 접힘 180 / 펼침 0.
        ImVec2 aCenter(wp.x + kWinW - 40.0f + 20.0f + 4.5f, wp.y + 18.0f + 2.0f);
        if (!DrawAssetRotated(dl, Asset::ExpandUp, aCenter, 9.0f, 4.0f,
                              cat->Expanded ? 0.0f : 180.0f, aCol))
            Arrow(dl, aCenter, 4.0f, 8.0f, cat->Expanded ? 270.0f : 90.0f, aCol);

        if (cat->Expanded) {
            // children: ScrollingFrame Size (1,0),(1,-41) @ (0,37)
            ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + 37.0f));
            ImGui::BeginChild("##catchildren", ImVec2(kWinW, winH - 41.0f), false,
                              ImGuiWindowFlags_NoScrollbar);
            float y = 0.0f;
            for (auto& m : cat->Modules) RenderModule(cat, m.get(), y);
            ImGui::Dummy(ImVec2(kWinW, y));
            float scrollY = ImGui::GetScrollY();
            ImGui::EndChild();

            // divider @ (0,37), 흰색 투명도 0.928 (alpha 18).
            // 원본은 CanvasPosition.Y > 10 일 때만 보인다 = 내용이 위로 밀렸을 때.
            if (scrollY > 10.0f)
                dl->AddRectFilled(ImVec2(wp.x, wp.y + 37.0f),
                                  ImVec2(wp.x + kWinW, wp.y + 38.0f),
                                  IM_COL32(255, 255, 255, 18));
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ===========================================================================
// settingspane  (원본 gui.lua:630)
//   메인 창을 그대로 덮는다. 배경 Dark(Main, 0.02), 모서리 5
//   title 'Settings' 13px @ (36,11), back 16x16 @ (11,13), 닫기 버튼
//   children @ (0,41), 크기 (1,0),(1,-57)
//   version 10px, 오른쪽 정렬, 하단 16px
// ===========================================================================

void VapeGui::EnsureSettingsOptions() {
    if (themeColor_) return;

    themeColor_ = std::make_unique<Option>();
    themeColor_->Type = OptionType::ColorSlider;
    themeColor_->Name = "GUI Theme";
    themeColor_->Tooltip = "Changes the color of the GUI";
    themeColor_->Color.Hue = g_palette.Hue;
    themeColor_->Color.Sat = g_palette.Sat;
    themeColor_->Color.Value = g_palette.Value;
    themeColor_->Color.Rainbow = g_palette.Rainbow;

    rainbowSpeed_ = std::make_unique<Option>();
    rainbowSpeed_->Type = OptionType::Slider;
    rainbowSpeed_->Name = "Rainbow speed";
    rainbowSpeed_->Tooltip = "Adjusts the speed of rainbow values";
    rainbowSpeed_->Min = 0.1f;
    rainbowSpeed_->Max = 10.0f;
    rainbowSpeed_->Decimal = 10.0f;
    rainbowSpeed_->Value = g_palette.RainbowSpeed;
    rainbowSpeed_->Touched = true;

    guiThemeDropdown_ = std::make_unique<Option>();
    guiThemeDropdown_->Type = OptionType::Dropdown;
    guiThemeDropdown_->Name = "GUI Theme";
    guiThemeDropdown_->List = { "new", "old", "rise" };
    guiThemeDropdown_->Selected = "new";

    rainbowMode_ = std::make_unique<Option>();
    rainbowMode_->Type = OptionType::Dropdown;
    rainbowMode_->Name = "Rainbow Mode";
    rainbowMode_->List = { "Normal", "Gradient", "Retro" };
    rainbowMode_->Selected = g_palette.RainbowMode == 0 ? "Normal"
                                : (g_palette.RainbowMode == 2 ? "Retro" : "Gradient");

    rainbowUpdateRate_ = std::make_unique<Option>();
    rainbowUpdateRate_->Type = OptionType::Slider;
    rainbowUpdateRate_->Name = "Rainbow update rate";
    rainbowUpdateRate_->Min = 1.0f;
    rainbowUpdateRate_->Max = 144.0f;
    rainbowUpdateRate_->Value = 60.0f;
    rainbowUpdateRate_->Suffix = "hz";
}

void VapeGui::RenderSettingsPane(ImVec2 wp, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* pageNames[4] = { "General", "Modules", "GUI", "Notifications" };
    const char* title = settingsPage_ < 0 ? "Settings" : pageNames[settingsPage_];
    const ImU32 panelBg = settingsPage_ < 0 ? Dark(g_palette.Main, 0.02f) : g_palette.Main;
    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), panelBg, 5.0f);

    ImGui::SetCursorScreenPos(ImVec2(wp.x + 11.0f, wp.y + 13.0f));
    ImGui::InvisibleButton("##back", ImVec2(16, 16));
    bool backHover = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        if (settingsPage_ >= 0) settingsPage_ = -1;
        else settingsOpen_ = false;
    }
    ImU32 backCol = backHover ? g_palette.Text : Light(g_palette.Main, 0.37f);
    if (!DrawAssetSized(dl, Asset::Back, ImVec2(wp.x + 11.0f, wp.y + 13.0f), 16.0f, 16.0f, backCol))
        Arrow(dl, ImVec2(wp.x + 19.0f, wp.y + 21.0f), 4.0f, 8.0f, 180.0f, backCol);

    TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + 36.0f, wp.y + 11.0f),
           ImVec2(w - 36.0f, 20.0f), XAlign::Left, g_palette.Text, title);

    if (CloseButton(dl, wp, w)) {
        if (settingsPage_ >= 0) settingsPage_ = -1;
        else settingsOpen_ = false;
    }

    const float top = 41.0f;
    const float bottom = h - 16.0f;
    dl->AddRectFilled(ImVec2(wp.x, wp.y + top), ImVec2(wp.x + w, wp.y + bottom), g_palette.Main);
    dl->PushClipRect(ImVec2(wp.x, wp.y + top), ImVec2(wp.x + w, wp.y + bottom), true);
    dl->AddRectFilled(ImVec2(wp.x, wp.y + top), ImVec2(wp.x + w, wp.y + top + 1.0f),
                      IM_COL32(255, 255, 255, 18));

    float y = top + 1.0f;
    const ImU32 childBg = g_palette.Main;

    EnsureSettingsOptions();

    auto menuRow = [&](const char* name, int page) {
        ImVec2 o(wp.x, wp.y + y);
        ImGui::PushID(page);
        ImGui::SetCursorScreenPos(o);
        ImGui::InvisibleButton("##settingspage", ImVec2(w, 40.0f));
        bool hover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) settingsPage_ = page;
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + 40.0f),
                          hover ? Light(g_palette.Main, 0.02f) : g_palette.Main);
        TextIn(dl, Font14(), 14.0f,
               ImVec2(o.x + HairIndent(Font14(), 14.0f, kToggleHair, kToggleTextFallback), o.y),
               ImVec2(w, 40.0f), XAlign::Left,
               hover ? g_palette.Text : Dark(g_palette.Text, 0.16f), name);
        Arrow(dl, ImVec2(o.x + w - 18.0f, o.y + 20.0f), 4.0f, 8.0f, 0.0f,
              Light(g_palette.Main, 0.37f));
        ImGui::PopID();
        y += 40.0f;
    };

    auto buttonRow = [&](const char* name, const char* tooltip) {
        ImVec2 o(wp.x, wp.y + y);
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + 31.0f), childBg);
        ImGui::SetCursorScreenPos(o);
        ImGui::PushID(name);
        ImGui::InvisibleButton("##settingsbutton", ImVec2(w, 31.0f));
        bool bh = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        Tip(tooltip);
        ImVec2 pMin(o.x + 10.0f, o.y + 2.0f), pMax(pMin.x + w - 20.0f, pMin.y + 27.0f);
        dl->AddRectFilled(pMin, pMax,
                          Mix(Light(g_palette.Main, 0.05f), Light(g_palette.Main, 0.0875f),
                              Tween01(ImGui::GetID("##buttonhover"), bh)), 5.0f);
        dl->AddRectFilled(ImVec2(pMin.x + 2, pMin.y + 2), ImVec2(pMax.x - 2, pMax.y - 2),
                          g_palette.Main, 4.0f);
        TextIn(dl, Font14(), 14.0f, ImVec2(pMin.x + 2.0f, pMin.y + 2.0f),
               ImVec2((pMax.x - pMin.x) - 4.0f, 23.0f), XAlign::Center,
               Dark(g_palette.Text, 0.16f), name);
        ImGui::PopID();
        y += 31.0f;
        return clicked;
    };

    auto rebindRow = [&] {
        ImVec2 o(wp.x, wp.y + y);
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + 40.0f), childBg);
        TextIn(dl, Font14(), 14.0f,
               ImVec2(o.x + HairIndent(Font14(), 14.0f, kToggleHair, kToggleTextFallback), o.y),
               ImVec2(w, 40.0f), XAlign::Left, Dark(g_palette.Text, 0.16f), "Rebind GUI");
        std::string bindText = JoinUpper(Keybind);
        float bw = ImMax(CalcTextF(g_fontBox, 12.0f, bindText.c_str()).x + 10.0f, 20.0f);
        ImVec2 bMin(o.x + w - 10.0f - bw, o.y + 9.0f);
        ImGui::SetCursorScreenPos(bMin);
        ImGui::InvisibleButton("##rebind", ImVec2(bw, 21.0f));
        bool bh = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) BeginGuiBinding();
        Tip("Click to bind");
        dl->AddRectFilled(bMin, ImVec2(bMin.x + bw, bMin.y + 21.0f),
                          IM_COL32(255, 255, 255, 20), 4.0f);
        if (BindingGui) {
            TextIn(dl, g_fontBox, 12.0f, ImVec2(bMin.x, bMin.y + 1.0f), ImVec2(bw, 21.0f),
                   XAlign::Center, Dark(g_palette.Text, 0.16f), "...");
        } else if (bh) {
            DrawAssetSized(dl, Asset::Edit, ImVec2(bMin.x + bw * 0.5f - 6.0f, bMin.y + 5.0f),
                           12.0f, 12.0f, Dark(g_palette.Text, 0.16f));
        } else {
            TextIn(dl, g_fontBox, 12.0f, ImVec2(bMin.x, bMin.y + 1.0f), ImVec2(bw, 21.0f),
                   XAlign::Center, Dark(g_palette.Text, 0.43f), bindText.c_str());
        }
        y += 40.0f;
    };

    auto toggleSetting = [&](const char* name, bool& value, int index, const char* tooltip,
                             ImU32 bg = 0) {
        ImGui::PushID(name);
        const bool clicked = ToggleRow(ImVec2(wp.x, wp.y + y), w, name, value,
                                       bg ? bg : childBg, index, tooltip);
        ImGui::PopID();
        y += 30.0f;
        return clicked;
    };

    ImGui::PushID("##settingscontent");
    if (settingsPage_ < 0) {
        for (int i = 0; i < 4; i++) menuRow(pageNames[i], i);
        RenderOption(themeColor_.get(), w, y, false, childBg);
        rebindRow();
    } else if (settingsPage_ == 0) {
        toggleSetting("Enable Multi-Keybinding", MultiKeybind, 0,
                      "Allows multiple keys to be bound to a module (eg. G + H)");
        if (buttonRow("Reset current profile",
                      "This will set your profile to the default settings of Vape")) {
            errno = 0;
            if (std::remove(ConfigPath.c_str()) == 0 || errno == ENOENT) {
                SkipSaveOnExit = true;
                RestartRequested = ExitRequested = true;
            } else {
                CreateNotification("Vape", "Failed to reset profile.", 10.0f,
                                   NotificationType::Alert);
            }
        }
        if (buttonRow("Self destruct", "Removes vape from the current game")) {
            CancelBinding();
            SkipSaveOnExit = true;
            ExitRequested = true;
        }
        if (buttonRow("Reinject", "Reloads vape for debugging purposes")) {
            if (Save()) {
                SkipSaveOnExit = true;
                RestartRequested = ExitRequested = true;
            } else {
                CreateNotification("Vape", "Failed to save profile.", 10.0f,
                                   NotificationType::Alert);
            }
        }
    } else if (settingsPage_ == 1) {
        toggleSetting("Teams by server", teamsByServer_, 0,
                      "Ignore players on your team designated by the server");
        toggleSetting("Use team color", useTeamColor_, 1,
                      "Uses the TeamColor property on players for render modules");
    } else if (settingsPage_ == 2) {
        toggleSetting("Blur background", blurBackground_, 0, "Blur the background of the GUI");
        toggleSetting("GUI bind indicator", guiBindIndicator_, 1,
                      "Displays a message indicating your GUI bind");
        toggleSetting("Show tooltips", ShowTooltips, 2, "Toggles visibility of these");
        toggleSetting("Show legit mode", showLegit_, 3,
                      "Shows the button to change to Legit Mode");
        toggleSetting("Auto rescale", autoRescale_, 4,
                      "Automatically rescales the GUI using the screen resolution");
        const bool themeWasExpanded = guiThemeDropdown_->Expanded;
        RenderOption(guiThemeDropdown_.get(), w, y, false, childBg);
        if (guiThemeDropdown_->Expanded && !themeWasExpanded)
            rainbowMode_->Expanded = false;
        const bool rainbowWasExpanded = rainbowMode_->Expanded;
        RenderOption(rainbowMode_.get(), w, y, false, childBg);
        if (rainbowMode_->Expanded && !rainbowWasExpanded)
            guiThemeDropdown_->Expanded = false;
        RenderOption(rainbowSpeed_.get(), w, y, false, childBg);
        RenderOption(rainbowUpdateRate_.get(), w, y, false, childBg);
        if (buttonRow("Reset GUI positions", "This will reset your GUI back to default")) {
            MainPosition = ImVec2(6, 60);
            for (auto& c : Categories) c->Position = ImVec2(236, 60);
            for (auto& list : categoryLists_) list.Position = ImVec2(240, 46);
            resetPositions_ = true;
        }
        if (buttonRow("Sort GUI", "Sorts GUI")) {
            int visibleIndex = 1;
            for (auto& c : Categories) if (c->Visible) {
                c->Position = ImVec2(6.0f + (visibleIndex % 8) * 230.0f,
                                     60.0f + (visibleIndex > 7 ? 360.0f : 0.0f));
                visibleIndex++;
            }
            resetPositions_ = true;
        }
    } else {
        toggleSetting("Notifications", notifications_, 0, "Shows notifications");
        if (notifications_)
            toggleSetting("Toggle alert", toggleAlert_, 1,
                          "Notifies you if a module is enabled/disabled.", Dark(childBg, 0.02f));
    }
    ImGui::PopID();

    g_palette.Rainbow = themeColor_->Color.Rainbow;
    if (!g_palette.Rainbow) g_palette.Hue = themeColor_->Color.Hue;
    g_palette.Sat = themeColor_->Color.Sat;
    g_palette.Value = themeColor_->Color.Value;
    g_palette.RainbowSpeed = rainbowSpeed_->Value;
    g_palette.RainbowMode = rainbowMode_->Selected == "Normal" ? 0
                              : (rainbowMode_->Selected == "Retro" ? 2 : 1);

    if (guiThemeDropdown_->Selected == "old") {
        g_palette.Main = IM_COL32(22, 22, 23, 255);
        g_palette.Text = IM_COL32(212, 212, 212, 255);
    } else if (guiThemeDropdown_->Selected == "rise") {
        g_palette.Main = IM_COL32(31, 29, 32, 255);
        g_palette.Text = IM_COL32(232, 232, 232, 255);
    } else {
        g_palette.Main = IM_COL32(26, 25, 26, 255);
        g_palette.Text = IM_COL32(200, 200, 200, 255);
    }
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = V4(g_palette.Main);

    dl->PopClipRect();
    settingsHeight_ = 506.0f;

    TextIn(dl, g_fontSmall, 10.0f, ImVec2(wp.x, wp.y + h - 16.0f), ImVec2(w - 2.0f, 16.0f),
           XAlign::Right, Dark(g_palette.Text, 0.43f), "Vape V4 (ImGui) ");
}

// ===========================================================================
// 메인 창  (원본 2468 mainapi:CreateGUI)
//   window @ (6,60), 배경 Dark(Main, 0.02)
//   logo 62x18 @ (11,10), v4 로고 28x16
//   settings 40x40 @ (1,-40), discord 16x16 @ (1,-56),(0,11)
//   children @ (0,37), 카테고리 버튼 220x40
// ===========================================================================

void VapeGui::RenderMainWindow() {
    // Divider(1) + 7 category rows + MISC(27) + 3 category-list rows
    // + overlays bar(36). 원본은 최종 높이에 42px 를 더한다.
    const float contentH = 1.0f + Categories.size() * 40.0f + 27.0f + 120.0f + 36.0f;
    float h = std::min(42.0f + contentH, 605.0f);
    if (settingsOpen_) h = std::min(std::max(h, settingsHeight_), 605.0f);

    ImGui::SetNextWindowPos(MainPosition, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kWinW, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Dark(g_palette.Main, 0.02f));

    bool settingsJustOpened = false;
    const bool blockBaseInput = settingsOpen_ || overlaysOpen_;
    if (ImGui::Begin("##vapemain", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        MainPosition = wp;
        Blur(wp, ImVec2(wp.x + kWinW, wp.y + h));
        bool baseDisabledActive = false;
        if (blockBaseInput) {
            ImGui::BeginDisabled();
            baseDisabledActive = true;
        }

        // guivape.png 62x18 @ (11,10), guiv4.png 28x16 @ 로고 우측 (1,1)
        // 원본 logo.ImageColor3 = Main 의 밝기가 0.5 초과면 Text, 아니면 흰색
        if (!DrawAssetSized(dl, Asset::GuiVape, ImVec2(wp.x + 11.0f, wp.y + 10.0f),
                            62.0f, 18.0f, IM_COL32(255, 255, 255, 255))) {
            dl->AddText(ImVec2(wp.x + 11.0f, wp.y + 10.0f), g_palette.Text, "vape");
            float lw = ImGui::CalcTextSize("vape").x;
            dl->AddText(ImVec2(wp.x + 11.0f + lw + 4.0f, wp.y + 10.0f), g_palette.Accent(), "v4");
        } else {
            DrawAssetSized(dl, Asset::GuiV4, ImVec2(wp.x + 11.0f + 63.0f, wp.y + 11.0f),
                           28.0f, 16.0f, IM_COL32(255, 255, 255, 255));
        }

        // guisettings.png 14x14, settingsbutton 40x40 @ (kWinW-40, 0)
        ImGui::SetCursorScreenPos(ImVec2(wp.x + kWinW - 40.0f, wp.y));
        ImGui::InvisibleButton("##set", ImVec2(40, 40));
        bool sHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            settingsOpen_ = true;
            settingsPage_ = -1;
            settingsJustOpened = true;
        }
        Tip("Open settings");
        // guisettings.png 14x14 @ settingsbutton 내 (15,12)
        ImU32 sCol = sHover ? g_palette.Text : Light(g_palette.Main, 0.37f);
        if (!DrawAssetSized(dl, Asset::GuiSettings,
                            ImVec2(wp.x + kWinW - 40.0f + 15.0f, wp.y + 12.0f), 14.0f, 14.0f, sCol))
            CatIcon(dl, 9, ImVec2(wp.x + kWinW - 40.0f + 22.0f, wp.y + 19.0f), 7.0f, sCol);

        // discord.png 16x16 @ (kWinW-56, 11)
        ImGui::SetCursorScreenPos(ImVec2(wp.x + kWinW - 56.0f, wp.y + 11.0f));
        ImGui::InvisibleButton("##dc", ImVec2(16, 16));
        // 원본은 Discord RPC 로 초대를 띄운다. 여기서는 초대 링크를 연다.
        if (ImGui::IsItemClicked()) OpenDiscordInvite();
        Tip("Join discord");
        if (!DrawAssetSized(dl, Asset::Discord, ImVec2(wp.x + kWinW - 56.0f, wp.y + 11.0f),
                            16.0f, 16.0f, IM_COL32(255, 255, 255, 255)))
            dl->AddCircle(ImVec2(wp.x + kWinW - 48.0f, wp.y + 19.0f), 7.0f,
                          Light(g_palette.Main, 0.37f), 12, 1.4f);

        // children @ y=37. 첫 항목은 1px divider.
        float y = 37.0f;
        dl->AddRectFilled(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + kWinW, wp.y + y + 1.0f),
                          Light(g_palette.Main, 0.02f));
        y += 1.0f;

        for (auto& cat : Categories) {
            ImVec2 o(wp.x, wp.y + y);
            ImGui::PushID(cat.get());
            ImGui::SetCursorScreenPos(o);
            ImGui::InvisibleButton("##catbtn", ImVec2(kWinW, 40.0f));
            bool bh = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) cat->Visible = !cat->Visible;

            // 원본 CreateButton: 켜짐/호버시 Light(Main, 0.02), 기본 Main
            bool lit = cat->Visible || bh;
            dl->AddRectFilled(o, ImVec2(o.x + kWinW, o.y + 40.0f),
                              lit ? Light(g_palette.Main, 0.02f) : g_palette.Main);

            // 글자: 켜짐 = 강조색 / 호버 = Text / 기본 = Dark(Text, 0.16)
            ImU32 tc = cat->Visible ? g_palette.Accent()
                                    : (bh ? g_palette.Text : Dark(g_palette.Text, 0.16f));
            // 아이콘 색은 Toggle 시점에만 갱신된다. 원본의 MouseEnter/Leave 는
            // 존재하지 않는 변수(buttonicon)를 건드리므로 호버에 반응하지 않는다.
            ImU32 ic = cat->Visible ? g_palette.Accent() : Dark(g_palette.Text, 0.16f);

            // icon @ (13,13), 크기는 카테고리마다 다르다
            const CatIconDef* def = FindCatIcon(cat->Name);
            const float iw = def ? def->w : 14.0f, ih = def ? def->h : 14.0f;
            if (!DrawCatIconTex(dl, cat->Name,
                                ImVec2(o.x + 13.0f + iw * 0.5f, o.y + 13.0f + ih * 0.5f), ic))
                CatIcon(dl, cat->IconIndex, ImVec2(o.x + 13.0f + 7.0f, o.y + 13.0f + 7.0f), 7.0f, ic);

            // 이름: HAIR SPACE 33칸(아이콘 있음) / 13칸(없음), TextSize 14
            const int hair = def ? kCatHair : kCatHairNoIcon;
            float indent = HairIndent(Font14(), 14.0f, hair, hair * 14.0f * kHairPerPx);
            TextIn(dl, Font14(), 14.0f, ImVec2(o.x + indent, o.y), ImVec2(kWinW - indent, 40.0f),
                   XAlign::Left, tc, cat->Name.c_str());

            // expandright.png 4x8 @ (1,-20),(0,16), 열림시 -14 로 이동 (회전 없음)
            float ax = cat->Visible ? 14.0f : 20.0f;
            Arrow(dl, ImVec2(o.x + kWinW - ax + 2.0f, o.y + 20.0f), 4.0f, 8.0f, 0.0f,
                  Light(g_palette.Main, 0.37f));

            ImGui::PopID();
            y += 40.0f;
        }

        // CreateDivider('misc'): 27px label, 1px divider at y=26.
        TextIn(dl, g_fontSmall, 9.0f,
               ImVec2(wp.x + HairIndent(g_fontSmall, 9.0f, 10, 8.0f), wp.y + y),
               ImVec2(kWinW, 27.0f), XAlign::Left, Dark(g_palette.Text, 0.43f), "MISC");
        dl->AddRectFilled(ImVec2(wp.x, wp.y + y + 26.0f),
                          ImVec2(wp.x + kWinW, wp.y + y + 27.0f),
                          Light(g_palette.Main, 0.02f));
        y += 27.0f;

        // Friends / Profiles / Targets are CategoryList buttons. Their source
        // settings do not pass CategoryIcon, so the main rows intentionally
        // have text only.
        for (int i = 0; i < 3; i++) {
            CategoryListState& list = categoryLists_[i];
            ImVec2 o(wp.x, wp.y + y);
            ImGui::PushID(&list);
            ImGui::SetCursorScreenPos(o);
            ImGui::InvisibleButton("##listbtn", ImVec2(kWinW, 40.0f));
            bool bh = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) list.Visible = !list.Visible;

            dl->AddRectFilled(o, ImVec2(o.x + kWinW, o.y + 40.0f),
                              (list.Visible || bh) ? Light(g_palette.Main, 0.02f)
                                                   : g_palette.Main);
            ImU32 tc = list.Visible ? g_palette.Accent()
                                    : (bh ? g_palette.Text : Dark(g_palette.Text, 0.16f));
            float indent = HairIndent(Font14(), 14.0f, kCatHairNoIcon,
                                      kCatHairNoIcon * 14.0f * kHairPerPx);
            TextIn(dl, Font14(), 14.0f, ImVec2(o.x + indent, o.y),
                   ImVec2(kWinW - indent, 40.0f), XAlign::Left, tc, list.Name.c_str());

            if (i == 1) {
                ImVec2 pMin(o.x + 131.0f, o.y + 8.0f);
                dl->AddRectFilled(pMin, ImVec2(pMin.x + 53.0f, pMin.y + 24.0f),
                                  Light(g_palette.Main, 0.04f), 5.0f);
                TextIn(dl, g_fontBox, 12.0f, pMin, ImVec2(53.0f, 24.0f), XAlign::Center,
                       Dark(g_palette.Text, 0.29f),
                       Truncate(g_fontBox, 12.0f, activeProfile_, 45.0f).c_str());
            }

            float ax = list.Visible ? 14.0f : 20.0f;
            Arrow(dl, ImVec2(o.x + kWinW - ax + 2.0f, o.y + 20.0f), 4.0f, 8.0f, 0.0f,
                  Light(g_palette.Main, 0.37f));
            ImGui::PopID();
            y += 40.0f;
        }

        // CreateOverlayBar: top divider and 24x24 overlays button.
        ImVec2 bar(wp.x, wp.y + y);
        dl->AddRectFilled(bar, ImVec2(bar.x + kWinW, bar.y + 36.0f), g_palette.Main);
        dl->AddRectFilled(bar, ImVec2(bar.x + kWinW, bar.y + 1.0f), Light(g_palette.Main, 0.02f));
        ImVec2 ovMin(bar.x + kWinW - 29.0f, bar.y + 7.0f);
        ImGui::SetCursorScreenPos(ovMin);
        ImGui::InvisibleButton("##overlays", ImVec2(24.0f, 24.0f));
        bool ovHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) overlaysOpen_ = !overlaysOpen_;
        Tip("Open overlays menu");
        float ovBg = Tween01(ImGui::GetID("##overlaysbg"), ovHover);
        dl->AddRectFilled(ovMin, ImVec2(ovMin.x + 24.0f, ovMin.y + 24.0f),
                          IM_COL32(255, 255, 255, (int)(ovBg * 0.1f * 255.0f)), 12.0f);
        DrawAssetSized(dl, Asset::OverlaysIcon, ovMin, 24.0f, 24.0f,
                       ovHover ? g_palette.Text : Light(g_palette.Main, 0.37f));

        if (overlaysOpen_ && !settingsOpen_) {
            const float menuH = 42.0f + 3.0f * 40.0f;
            ImVec2 menuMin(wp.x, wp.y + h - menuH);
            if (baseDisabledActive) {
                ImGui::EndDisabled();
                baseDisabledActive = false;
            }
            dl->AddRectFilled(wp, ImVec2(wp.x + kWinW, wp.y + h - 5.0f),
                              IM_COL32(0, 0, 0, 128), 5.0f);
            ImGui::SetCursorScreenPos(menuMin);
            ImGui::BeginChild("##overlaylayer", ImVec2(kWinW, menuH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImDrawList* parentDl = dl;
            dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(menuMin, ImVec2(menuMin.x + kWinW, menuMin.y + menuH),
                              g_palette.Main, 5.0f);
            DrawAssetSized(dl, Asset::OverlaysTab, ImVec2(menuMin.x + 10.0f, menuMin.y + 13.0f),
                           14.0f, 12.0f, g_palette.Text);
            TextIn(dl, g_fontItem, 15.0f, ImVec2(menuMin.x + 36.0f, menuMin.y),
                   ImVec2(kWinW - 36.0f, 38.0f), XAlign::Left, g_palette.Text, "Overlays");
            ImGui::PushID("##overlaymenu");
            if (CloseButton(dl, menuMin, kWinW, 7.0f)) overlaysOpen_ = false;
            dl->AddRectFilled(ImVec2(menuMin.x, menuMin.y + 37.0f),
                              ImVec2(menuMin.x + kWinW, menuMin.y + 38.0f),
                              Light(g_palette.Main, 0.02f));
            const char* names[3] = { "Text GUI", "Radar", "Target Info" };
            const Asset icons[3] = { Asset::TextGuiIcon, Asset::RadarIcon, Asset::TargetInfoIcon };
            const ImVec2 sizes[3] = { ImVec2(16, 12), ImVec2(16, 16), ImVec2(17, 16) };
            for (int i = 0; i < 3; i++) {
                ImGui::PushID(i);
                ImVec2 row(menuMin.x, menuMin.y + 38.0f + i * 40.0f);
                ToggleRow(ImVec2(row.x, row.y + 5.0f), kWinW, "",
                          overlayToggles_[i], g_palette.Main, i, nullptr);
                DrawAssetSized(dl, icons[i],
                               ImVec2(row.x + 13.0f, row.y + (40.0f - sizes[i].y) * 0.5f),
                               sizes[i].x, sizes[i].y, g_palette.Text);
                TextIn(dl, Font14(), 14.0f, ImVec2(row.x + 38.0f, row.y + 5.0f),
                       ImVec2(kWinW - 78.0f, 30.0f), XAlign::Left,
                       Dark(g_palette.Text, 0.16f), names[i]);
                ImGui::PopID();
            }
            ImGui::PopID();
            ImGui::EndChild();
            dl = parentDl;
        }

        // settingspane 은 메인 창 위를 통째로 덮는다.
        if (settingsOpen_ && !settingsJustOpened) {
            if (baseDisabledActive) {
                ImGui::EndDisabled();
                baseDisabledActive = false;
            }
            ImGui::SetCursorScreenPos(wp);
            ImGui::BeginChild("##settingslayer", ImVec2(kWinW, h), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            RenderSettingsPane(wp, kWinW, h);
            ImGui::EndChild();
        }
        if (baseDisabledActive) {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void VapeGui::RenderSearchWindow() {
    struct SearchResult { Category* CategoryPtr; Module* ModulePtr; };
    std::vector<SearchResult> results;
    std::string query = searchBuffer_;
    std::transform(query.begin(), query.end(), query.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (!query.empty()) {
        for (auto& cat : Categories) {
            for (auto& mod : cat->Modules) {
                std::string name = mod->Name;
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (name.find(query) != std::string::npos)
                    results.push_back({cat.get(), mod.get()});
            }
        }
    }

    const int shown = std::min((int)results.size(), 10);
    const float h = 37.0f + shown * 40.0f;
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 110.0f, 13.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kWinW, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Dark(g_palette.Main, 0.02f));
    if (ImGui::Begin("##vapesearch", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        Blur(wp, ImVec2(wp.x + kWinW, wp.y + h));

        if (showLegit_) {
            ImVec2 legitMin(wp.x + 8.0f, wp.y + 11.0f);
            ImGui::SetCursorScreenPos(legitMin);
            ImGui::InvisibleButton("##legit", ImVec2(29.0f, 16.0f));
            if (ImGui::IsItemClicked()) legitOpen_ = true;
            DrawAssetSized(dl, Asset::Legit, legitMin, 29.0f, 16.0f,
                           IM_COL32(255, 255, 255, 255));
            dl->AddRectFilled(ImVec2(wp.x + 43.0f, wp.y + 13.0f),
                              ImVec2(wp.x + 45.0f, wp.y + 25.0f), Light(g_palette.Main, 0.14f));
        }

        if (g_fontBox) ImGui::PushFont(g_fontBox);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + (showLegit_ ? 50.0f : 10.0f), wp.y + 6.0f));
        ImGui::SetNextItemWidth(showLegit_ ? 145.0f : 185.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 5));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, g_palette.Text);
        ImGui::InputText("##searchinput", searchBuffer_, sizeof(searchBuffer_));
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (g_fontBox) ImGui::PopFont();
        DrawAssetSized(dl, Asset::Search, ImVec2(wp.x + kWinW - 23.0f, wp.y + 11.0f),
                       14.0f, 14.0f, Light(g_palette.Main, 0.37f));

        for (int i = 0; i < shown; i++) {
            Module* mod = results[i].ModulePtr;
            ImVec2 row(wp.x, wp.y + 34.0f + i * 40.0f);
            ImGui::PushID(mod);
            ImGui::SetCursorScreenPos(row);
            ImGui::InvisibleButton("##searchrow", ImVec2(kWinW, 40.0f),
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ToggleModule(mod);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                results[i].CategoryPtr->Visible = true;
                results[i].CategoryPtr->Expanded = true;
                mod->ChildrenVisible = true;
            }
            dl->AddRectFilled(row, ImVec2(row.x + kWinW, row.y + 40.0f),
                              hovered ? Light(g_palette.Main, 0.02f) : g_palette.Main);
            ImU32 tc = mod->Enabled ? g_palette.AccentIndexed(mod->Index)
                                    : (hovered ? g_palette.Text : Dark(g_palette.Text, 0.16f));
            TextIn(dl, Font14(), 14.0f,
                   ImVec2(row.x + HairIndent(Font14(), 14.0f, kModuleHair, kModuleTextFallback), row.y),
                   ImVec2(kWinW, 40.0f), XAlign::Left, tc, mod->Name.c_str());
            Dots(dl, ImVec2(row.x + kWinW - 21.0f, row.y + 12.0f),
                 mod->Enabled ? IM_COL32(50, 50, 50, 255) : Light(g_palette.Main, 0.37f));
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void VapeGui::RenderLegitWindow() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 350.0f,
                                   ImGui::GetIO().DisplaySize.y * 0.5f - 194.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700.0f, 389.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, g_palette.Main);
    if (ImGui::Begin("##vapelegit", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        Blur(wp, ImVec2(wp.x + 700.0f, wp.y + 389.0f));
        DrawAssetSized(dl, Asset::LegitTab, ImVec2(wp.x + 18.0f, wp.y + 13.0f),
                       16.0f, 16.0f, g_palette.Text);
        TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + 44.0f, wp.y + 7.0f),
               ImVec2(580.0f, 28.0f), XAlign::Left, g_palette.Text, "Legit Mode");
        if (CloseButton(dl, wp, 700.0f)) legitOpen_ = false;
        dl->AddRectFilled(ImVec2(wp.x, wp.y + 40.0f), ImVec2(wp.x + 700.0f, wp.y + 41.0f),
                          Light(g_palette.Main, 0.05f));

        ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + 41.0f));
        ImGui::BeginChild("##legitcontent", ImVec2(700.0f, 348.0f), false);
        ImDrawList* parentDl = dl;
        dl = ImGui::GetWindowDrawList();
        const ImVec2 contentWp = ImGui::GetWindowPos();
        const float contentBaseY = contentWp.y - ImGui::GetScrollY();
        int item = 0;
        for (auto& cat : Categories) {
            for (auto& mod : cat->Modules) {
                const int col = item % 3, row = item / 3;
                ImVec2 o(contentWp.x + 17.0f + col * 227.0f,
                         contentBaseY + 11.0f + row * 64.0f);
                ImGui::PushID(mod.get());
                ImGui::SetCursorScreenPos(o);
                ImGui::InvisibleButton("##legitmodule", ImVec2(211.0f, 52.0f),
                                       ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonRight);
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ToggleModule(mod.get());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    legitOpen_ = false;
                    cat->Visible = true;
                    cat->Expanded = true;
                    mod->ChildrenVisible = true;
                }
                dl->AddRectFilled(o, ImVec2(o.x + 211.0f, o.y + 52.0f),
                                  hovered ? Light(g_palette.Main, 0.06f)
                                          : Light(g_palette.Main, 0.025f), 5.0f);
                if (mod->Enabled)
                    dl->AddRectFilled(o, ImVec2(o.x + 3.0f, o.y + 52.0f),
                                      g_palette.AccentIndexed(mod->Index), 3.0f);
                TextIn(dl, Font14(), 14.0f, ImVec2(o.x + 13.0f, o.y + 5.0f),
                       ImVec2(160.0f, 23.0f), XAlign::Left,
                       mod->Enabled ? g_palette.AccentIndexed(mod->Index)
                                    : Dark(g_palette.Text, 0.10f), mod->Name.c_str());
                TextIn(dl, g_fontSmall, 11.0f, ImVec2(o.x + 13.0f, o.y + 27.0f),
                       ImVec2(160.0f, 18.0f), XAlign::Left,
                       Dark(g_palette.Text, 0.43f), cat->Name.c_str());
                ImVec2 toggle(o.x + 181.0f, o.y + 20.0f);
                dl->AddCircleFilled(toggle, 7.0f,
                                    mod->Enabled ? g_palette.AccentIndexed(mod->Index)
                                                 : Light(g_palette.Main, 0.18f), 20);
                dl->AddCircleFilled(toggle, 3.0f, g_palette.Main, 16);
                ImGui::PopID();
                item++;
            }
        }
        const float contentH = 11.0f + ((item + 2) / 3) * 64.0f;
        ImGui::SetCursorPos(ImVec2(0.0f, contentH));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ImGui::EndChild();
        dl = parentDl;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void VapeGui::RenderCategoryListWindow(CategoryListState& list, int index) {
    const float contentH = 31.0f + list.Entries.size() * 35.0f;
    const float settingsContentH = index == 1 ? 82.0f : 92.0f;
    const float h = list.SettingsVisible ? 45.0f + settingsContentH
                    : (list.Expanded ? std::min(51.0f + contentH, 611.0f) : 45.0f);
    std::string id = "##vapelist" + list.Name;
    ImGui::SetNextWindowPos(list.Position, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kWinW, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, g_palette.Main);
    if (ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        list.Position = wp;
        Blur(wp, ImVec2(wp.x + kWinW, wp.y + h));

        Asset icon = index == 1 ? Asset::ProfilesIcon
                                : (index == 2 ? Asset::TargetsTab : Asset::FriendsTab);
        ImVec2 iconSize = index == 1 ? ImVec2(17, 10)
                                     : (index == 2 ? ImVec2(18, 12) : ImVec2(17, 16));
        ImVec2 iconPos = index == 1 ? ImVec2(12, 16)
                                    : (index == 2 ? ImVec2(11, 14) : ImVec2(12, 12));
        DrawAssetSized(dl, icon, ImVec2(wp.x + iconPos.x, wp.y + iconPos.y),
                       iconSize.x, iconSize.y, g_palette.Text);
        TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + 36.0f, wp.y + 12.0f),
               ImVec2(kWinW - 36.0f, 20.0f), XAlign::Left, g_palette.Text, list.Name.c_str());

        ImVec2 settingsMin(wp.x + kWinW - 52.0f, wp.y + 13.0f);
        ImGui::SetCursorScreenPos(settingsMin);
        ImGui::InvisibleButton("##listsettings", ImVec2(16, 16));
        bool settingsHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) list.SettingsVisible = !list.SettingsVisible;
        DrawAssetSized(dl, Asset::CustomSettings, settingsMin, 16.0f, 16.0f,
                       settingsHover ? g_palette.Text : Dark(g_palette.Text, 0.43f));

        ImGui::SetCursorScreenPos(ImVec2(wp.x + kWinW - 36.0f, wp.y));
        ImGui::InvisibleButton("##listarrow", ImVec2(36, 40),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool ah = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
            ImGui::IsItemClicked(ImGuiMouseButton_Right)) list.Expanded = !list.Expanded;
        ImU32 ac = ah ? IM_COL32(220, 220, 220, 255) : IM_COL32(140, 140, 140, 255);
        DrawAssetRotated(dl, Asset::ExpandUp, ImVec2(wp.x + 204.5f, wp.y + 21.0f),
                         9.0f, 4.0f, list.Expanded ? 0.0f : 180.0f, ac);

        if (list.SettingsVisible) {
            ImGui::PushID("##listsettingscontent");
            float sy = 45.0f;
            auto listToggle = [&](const char* name, bool& value, int color) {
                ImGui::PushID(name);
                ToggleRow(ImVec2(wp.x, wp.y + sy), kWinW, name, value, g_palette.Main,
                          color, nullptr);
                ImGui::PopID();
                sy += 30.0f;
            };
            if (index == 1) {
                listToggle("Auto save profile", list.AutoSave, 0);
                TextIn(dl, g_fontBox, 12.0f, ImVec2(wp.x + 12.0f, wp.y + sy),
                       ImVec2(kWinW - 24.0f, 32.0f), XAlign::Left,
                       Dark(g_palette.Text, 0.16f),
                       ("Active: " + activeProfile_).c_str());
            } else {
                listToggle(index == 0 ? "Use friends" : "Use targets", list.UseList, 0);
                listToggle("Recolor visuals", list.RecolorVisuals, 1);
            }
            ImGui::PopID();
        } else if (list.Expanded) {
            ImDrawList* parentDl = dl;
            ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + 45.0f));
            ImGui::BeginChild("##listcontent", ImVec2(kWinW, h - 45.0f), false);
            dl = ImGui::GetWindowDrawList();
            const ImVec2 contentWp = ImGui::GetWindowPos();
            const float contentBaseY = contentWp.y - ImGui::GetScrollY();

            ImVec2 addMin(contentWp.x + 10.0f, contentBaseY);
            ImVec2 addMax(addMin.x + 200.0f, addMin.y + 31.0f);
            dl->AddRectFilled(addMin, addMax, Light(g_palette.Main, 0.02f), 5.0f);
            dl->AddRectFilled(ImVec2(addMin.x + 1, addMin.y + 1),
                              ImVec2(addMax.x - 1, addMax.y - 1), Dark(g_palette.Main, 0.02f), 4.0f);
            if (g_fontItem) ImGui::PushFont(g_fontItem);
            ImGui::SetCursorScreenPos(ImVec2(addMin.x + 10.0f, addMin.y + 3.0f));
            ImGui::SetNextItemWidth(155.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            const char* hint = index == 1 ? "Type name" : "Roblox username";
            bool enter = ImGui::InputTextWithHint("##listadd", hint, list.AddBuffer,
                                                  sizeof(list.AddBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopStyleColor();
            if (g_fontItem) ImGui::PopFont();
            ImVec2 plusMin(addMax.x - 26.0f, addMin.y + 8.0f);
            ImGui::SetCursorScreenPos(plusMin);
            ImGui::InvisibleButton("##listplus", ImVec2(16, 16));
            bool plusHover = ImGui::IsItemHovered();
            bool add = ImGui::IsItemClicked() || enter;
            DrawAssetSized(dl, Asset::Add, plusMin, 16.0f, 16.0f,
                           U32(ImVec4(V4(index == 0 ? IM_COL32(5, 134, 105, 255)
                                                   : g_palette.Accent()).x,
                                      V4(index == 0 ? IM_COL32(5, 134, 105, 255)
                                                   : g_palette.Accent()).y,
                                      V4(index == 0 ? IM_COL32(5, 134, 105, 255)
                                                   : g_palette.Accent()).z,
                                      plusHover ? 1.0f : 0.7f)));
            if (add && list.AddBuffer[0]) {
                std::string value = list.AddBuffer;
                if (std::find(list.Entries.begin(), list.Entries.end(), value) == list.Entries.end()) {
                    list.Entries.push_back(value);
                    list.Enabled.push_back(true);
                    if (index == 1) {
                        std::fill(list.Enabled.begin(), list.Enabled.end(), false);
                        list.Enabled.back() = true;
                        activeProfile_ = value;
                    }
                }
                list.AddBuffer[0] = 0;
            }

            for (int i = 0; i < (int)list.Entries.size(); i++) {
                ImVec2 row(contentWp.x + 10.0f, contentBaseY + 34.0f + i * 35.0f);
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(row);
                ImGui::InvisibleButton("##listentry", ImVec2(200.0f, 32.0f));
                bool rh = ImGui::IsItemHovered();
                bool rc = ImGui::IsItemClicked();
                ImVec2 closeMin(row.x + 174.0f, row.y + 8.0f);
                const bool canRemove = !(index == 1 && list.Entries[i] == "default");
                const bool closeHover = canRemove && rh && ImGui::IsMouseHoveringRect(
                    closeMin, ImVec2(closeMin.x + 16.0f, closeMin.y + 16.0f));
                dl->AddRectFilled(row, ImVec2(row.x + 200.0f, row.y + 32.0f),
                                  Light(g_palette.Main, 0.02f), 5.0f);
                if (rh) dl->AddRectFilled(ImVec2(row.x + 1, row.y + 1),
                                          ImVec2(row.x + 199.0f, row.y + 31.0f),
                                          g_palette.Main, 4.0f);
                ImU32 dot = list.Enabled[i] ? (index == 0 ? IM_COL32(5, 134, 105, 255)
                                                           : g_palette.Accent())
                                              : Light(g_palette.Main, 0.37f);
                dl->AddCircleFilled(ImVec2(row.x + 15.0f, row.y + 17.0f), 5.5f, dot, 16);
                dl->AddCircleFilled(ImVec2(row.x + 15.0f, row.y + 17.0f), 4.0f,
                                    list.Enabled[i] ? dot : Light(g_palette.Main, 0.02f), 16);
                TextIn(dl, g_fontItem, 15.0f, ImVec2(row.x + 30.0f, row.y),
                       ImVec2(144.0f, 32.0f), XAlign::Left, Dark(g_palette.Text, 0.16f),
                       list.Entries[i].c_str());
                bool remove = rc && closeHover;
                if (canRemove) {
                    ImVec4 cc = V4(Light(g_palette.Text, 0.2f)); cc.w = 0.5f;
                    DrawAssetSized(dl, Asset::CloseMini, closeMin, 16.0f, 16.0f, U32(cc));
                }
                if (remove) {
                    const bool removedActive = index == 1 && list.Entries[i] == activeProfile_;
                    list.Entries.erase(list.Entries.begin() + i);
                    list.Enabled.erase(list.Enabled.begin() + i);
                    if (removedActive) {
                        activeProfile_ = "default";
                        auto def = std::find(list.Entries.begin(), list.Entries.end(), "default");
                        if (def == list.Entries.end()) {
                            list.Entries.insert(list.Entries.begin(), "default");
                            list.Enabled.insert(list.Enabled.begin(), true);
                        } else {
                            std::fill(list.Enabled.begin(), list.Enabled.end(), false);
                            list.Enabled[(size_t)std::distance(list.Entries.begin(), def)] = true;
                        }
                    }
                    ImGui::PopID();
                    i--;
                    continue;
                }
                if (rc && !closeHover) {
                    if (index == 1) {
                        std::fill(list.Enabled.begin(), list.Enabled.end(), false);
                        list.Enabled[i] = true;
                        activeProfile_ = list.Entries[i];
                    } else {
                        list.Enabled[i] = !list.Enabled[i];
                    }
                }
                ImGui::PopID();
            }
            ImGui::SetCursorPos(ImVec2(0.0f, contentH));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            ImGui::EndChild();
            dl = parentDl;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ===========================================================================

void VapeGui::Init() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 5.0f;      // addCorner 기본 UDim.new(0,5)
    s.FrameRounding = 4.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.ItemSpacing = ImVec2(0, 0);
    s.WindowPadding = ImVec2(0, 0);
    s.ScrollbarSize = 2.0f;       // children.ScrollBarThickness = 2

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = V4(g_palette.Main);
    c[ImGuiCol_ChildBg] = V4(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_Text] = V4(g_palette.Text);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    // ScrollBarImageTransparency = 0.75
    c[ImGuiCol_ScrollbarGrab] = V4(IM_COL32(255, 255, 255, 64));
    c[ImGuiCol_ScrollbarGrabHovered] = V4(IM_COL32(255, 255, 255, 90));
    c[ImGuiCol_ScrollbarGrabActive] = V4(IM_COL32(255, 255, 255, 110));

    categoryLists_[0].Name = "Friends";
    categoryLists_[1].Name = "Profiles";
    categoryLists_[2].Name = "Targets";
    categoryLists_[1].Entries = { "default" };
    categoryLists_[1].Enabled = { true };
    EnsureSettingsOptions();
}

void VapeGui::CreateNotification(const std::string& title, const std::string& text,
                                 float duration, NotificationType type) {
    if (!notifications_) return;

    Toast toast;
    toast.Title = title;
    toast.Text = text;
    toast.Duration = ImMax(duration, 0.0f);
    toast.Type = type;
    toast.StackOffset = 29.0f + 78.0f * (float)(toasts_.size() + 1);
    toast.StackFrom = toast.StackTarget = toast.StackOffset;
    toasts_.push_back(std::move(toast));
}

void VapeGui::ToggleModule(Module* mod, bool notify) {
    if (!mod) return;
    mod->Toggle();
    if (notify && toggleAlert_) {
        const std::string state = mod->Enabled
            ? "<font color='#5AFF5A'>Enabled</font>"
            : "<font color='#FF5A5A'>Disabled</font>";
        CreateNotification("Module Toggled",
                           mod->Name + "<font color='#FFFFFF'> has been </font>" + state +
                               "<font color='#FFFFFF'>!</font>",
                           0.75f);
    }
}

void VapeGui::RenderNotifications() {
    const float dt = ImGui::GetIO().DeltaTime;
    for (auto& toast : toasts_) toast.Age += dt;

    const size_t oldCount = toasts_.size();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [](const Toast& t) {
                                     return t.Age >= t.Duration + 0.2f;
                                 }),
                  toasts_.end());
    if (toasts_.size() != oldCount) {
        for (size_t i = 0; i < toasts_.size(); i++) {
            Toast& toast = toasts_[i];
            const float target = 29.0f + 78.0f * (float)(i + 1);
            if (toast.StackTarget != target) {
                toast.StackFrom = toast.StackOffset;
                toast.StackTarget = target;
                toast.StackTweenAge = 0.0f;
            }
        }
    }
    if (toasts_.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = autoRescale_ ? ImMax(display.x / 1920.0f, 0.6f) : 1.0f;
    ImFont* bodyFont = Font14();
    ImFont* titleFont = g_fontSemiBold ? g_fontSemiBold : bodyFont;

    for (auto& toast : toasts_) {
        if (toast.StackTweenAge < 0.4f) {
            toast.StackTweenAge = ImMin(toast.StackTweenAge + dt, 0.4f);
            toast.StackOffset = toast.StackFrom +
                (toast.StackTarget - toast.StackFrom) *
                    EaseOutExpo(toast.StackTweenAge / 0.4f);
        } else {
            toast.StackOffset = toast.StackTarget;
        }
    }

    // 원본 Global ZIndex: 모든 blur(Z1)를 먼저, 본체(Z5)를 나중에 그린다.
    for (const auto& toast : toasts_) {
        const std::string plain = StripRichText(toast.Text);
        const float fontSize = 14.0f * scale;
        const float w = ImMax(CalcTextF(bodyFont, fontSize, plain.c_str()).x + 80.0f * scale,
                              266.0f * scale);
        const float h = 75.0f * scale;
        const float entryAtExit = EaseOutExpo(toast.Duration / 0.4f);
        const float anchor = toast.Age < toast.Duration
            ? EaseOutExpo(toast.Age / 0.4f)
            : entryAtExit * (1.0f - EaseOutExpo((toast.Age - toast.Duration) / 0.4f));
        const ImVec2 min(display.x - w * anchor,
                         display.y - toast.StackOffset * scale);
        const ImVec2 max(min.x + w, min.y + h);
        DrawAssetSliced(dl, Asset::BlurNotif,
                        ImVec2(min.x - 48.0f * scale, min.y - 31.0f * scale),
                        ImVec2(max.x + 41.0f * scale, max.y + 21.0f * scale),
                        0.0f, 0.0f, 309.0f, 533.0f,
                        52.0f, 31.0f, 48.0f, 31.0f,
                        IM_COL32_WHITE, scale);
    }

    for (const auto& toast : toasts_) {
        const std::string plain = StripRichText(toast.Text);
        const float fontSize = 14.0f * scale;
        const float w = ImMax(CalcTextF(bodyFont, fontSize, plain.c_str()).x + 80.0f * scale,
                              266.0f * scale);
        const float h = 75.0f * scale;
        const float entryAtExit = EaseOutExpo(toast.Duration / 0.4f);
        const float anchor = toast.Age < toast.Duration
            ? EaseOutExpo(toast.Age / 0.4f)
            : entryAtExit * (1.0f - EaseOutExpo((toast.Age - toast.Duration) / 0.4f));
        const ImVec2 min(display.x - w * anchor,
                         display.y - toast.StackOffset * scale);
        const ImVec2 max(min.x + w, min.y + h);

        if (!DrawAssetSliced(dl, Asset::Notification, min, max,
                             0.0f, 0.0f, 15.0f, 15.0f,
                             7.0f, 7.0f, 6.0f, 6.0f,
                             IM_COL32_WHITE, scale)) {
            dl->AddRectFilled(min, max, IM_COL32(0, 0, 0, 173), 7.0f * scale,
                              ImDrawFlags_RoundCornersLeft);
        }

        Asset iconAsset = Asset::Info;
        if (toast.Type == NotificationType::Warning) iconAsset = Asset::Warning;
        else if (toast.Type == NotificationType::Alert) iconAsset = Asset::Alert;
        DrawAssetSized(dl, iconAsset,
                       ImVec2(min.x - 5.0f * scale, min.y - 8.0f * scale),
                       60.0f * scale, 60.0f * scale, IM_COL32(0, 0, 0, 128));
        DrawAssetSized(dl, iconAsset,
                       ImVec2(min.x - 6.0f * scale, min.y - 9.0f * scale),
                       60.0f * scale, 60.0f * scale, IM_COL32_WHITE);

        const std::string plainTitle = StripRichText(toast.Title);
        const ImVec2 titlePos(min.x + 46.0f * scale, min.y + 16.0f * scale);
        const ImU32 titleStroke = IM_COL32(255, 255, 255, 128);
        const float stroke = 0.3f * scale;
        TextF(dl, titleFont, fontSize, ImVec2(titlePos.x - stroke, titlePos.y),
              titleStroke, plainTitle.c_str());
        TextF(dl, titleFont, fontSize, ImVec2(titlePos.x + stroke, titlePos.y),
              titleStroke, plainTitle.c_str());
        TextF(dl, titleFont, fontSize, ImVec2(titlePos.x, titlePos.y - stroke),
              titleStroke, plainTitle.c_str());
        TextF(dl, titleFont, fontSize, ImVec2(titlePos.x, titlePos.y + stroke),
              titleStroke, plainTitle.c_str());
        DrawRichText(dl, titleFont, fontSize, titlePos, toast.Title,
                     IM_COL32(209, 209, 209, 255));

        TextF(dl, bodyFont, fontSize,
              ImVec2(min.x + 47.0f * scale, min.y + 44.0f * scale),
              IM_COL32(0, 0, 0, 128), plain.c_str());
        DrawRichText(dl, bodyFont, fontSize,
                     ImVec2(min.x + 46.0f * scale, min.y + 43.0f * scale),
                     toast.Text, IM_COL32(170, 170, 170, 255));

        const ImU32 progressColor = toast.Type == NotificationType::Alert
            ? IM_COL32(250, 50, 56, 255)
            : toast.Type == NotificationType::Warning
                ? IM_COL32(236, 129, 43, 255)
                : IM_COL32(220, 220, 220, 255);
        const float remaining = toast.Duration > 0.0f
            ? ImClamp(1.0f - toast.Age / toast.Duration, 0.0f, 1.0f)
            : 0.0f;
        dl->AddRectFilled(ImVec2(min.x + 3.0f * scale, min.y + 71.0f * scale),
                          ImVec2(min.x + 3.0f * scale + (w - 13.0f * scale) * remaining,
                                 min.y + 73.0f * scale),
                          progressColor);
    }
}

void VapeGui::RenderOverlays() {
    const char* ids[3] = { "##vapetextgui", "##vaperadar", "##vapetargetinfo" };
    const ImVec2 sizes[3] = { ImVec2(210, 220), ImVec2(180, 205), ImVec2(250, 105) };
    const char* titles[3] = { "Text GUI", "Radar", "Target Info" };

    for (int index = 0; index < 3; index++) {
        if (!overlayToggles_[index]) continue;
        ImGui::SetNextWindowPos(overlayPositions_[index], ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(sizes[index], ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Dark(g_palette.Main, 0.02f));
        if (ImGui::Begin(ids[index], nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            overlayPositions_[index] = wp;
            Blur(wp, ImVec2(wp.x + sizes[index].x, wp.y + sizes[index].y));
            TextIn(dl, g_fontTitle, 13.0f, ImVec2(wp.x + 12, wp.y + 7),
                   ImVec2(sizes[index].x - 44, 26), XAlign::Left, g_palette.Text, titles[index]);
            if (CloseButton(dl, wp, sizes[index].x, 5.0f)) overlayToggles_[index] = false;
            dl->AddRectFilled(ImVec2(wp.x, wp.y + 37), ImVec2(wp.x + sizes[index].x, wp.y + 38),
                              Light(g_palette.Main, 0.05f));

            if (index == 0) {
                float y = wp.y + 45.0f;
                int shown = 0;
                for (auto& cat : Categories)
                    for (auto& mod : cat->Modules)
                        if (mod->Enabled && shown++ < 9) {
                            std::string label = mod->Name;
                            if (!mod->ExtraText.empty()) label += " " + mod->ExtraText;
                            TextIn(dl, g_fontBox, 12.0f, ImVec2(wp.x + 12, y),
                                   ImVec2(sizes[index].x - 24, 18), XAlign::Right,
                                   g_palette.AccentIndexed(mod->Index), label.c_str());
                            y += 18.0f;
                        }
                if (shown == 0)
                    TextIn(dl, g_fontBox, 12.0f, ImVec2(wp.x + 12, y),
                           ImVec2(sizes[index].x - 24, 24), XAlign::Center,
                           Dark(g_palette.Text, 0.43f), "No modules enabled");
            } else if (index == 1) {
                ImVec2 c(wp.x + sizes[index].x * 0.5f, wp.y + 120.0f);
                const float r = 67.0f;
                dl->AddCircle(c, r, Light(g_palette.Main, 0.20f), 48, 1.0f);
                dl->AddCircle(c, r * 0.5f, Light(g_palette.Main, 0.12f), 48, 1.0f);
                dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y),
                            Light(g_palette.Main, 0.12f));
                dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r),
                            Light(g_palette.Main, 0.12f));
                dl->AddCircleFilled(c, 3.0f, g_palette.Accent(), 16);
                TextIn(dl, g_fontSmall, 11.0f, ImVec2(wp.x, wp.y + 187),
                       ImVec2(sizes[index].x, 14), XAlign::Center,
                       Dark(g_palette.Text, 0.43f), "No entity data");
            } else {
                TextIn(dl, g_fontItem, 15.0f, ImVec2(wp.x + 14, wp.y + 45),
                       ImVec2(sizes[index].x - 28, 22), XAlign::Left,
                       Dark(g_palette.Text, 0.16f), "No target");
                ImVec2 bar(wp.x + 14, wp.y + 76);
                dl->AddRectFilled(bar, ImVec2(bar.x + sizes[index].x - 28, bar.y + 5),
                                  Light(g_palette.Main, 0.12f), 3.0f);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

void VapeGui::OpenSettings() {
    settingsOpen_ = true;
    settingsPage_ = -1;
}

bool VapeGui::OpenCategory(const std::string& name, bool expand) {
    for (auto& category : Categories) {
        if (category->Name != name) continue;
        category->Visible = true;
        if (expand) category->Expanded = true;
        return true;
    }
    for (auto& list : categoryLists_) {
        if (list.Name != name) continue;
        list.Visible = true;
        if (expand) list.Expanded = true;
        return true;
    }
    return false;
}

void VapeGui::ApplyStoredWindowPositions() {
    ImGui::SetWindowPos("##vapemain", MainPosition, ImGuiCond_Always);
    for (const auto& cat : Categories) {
        const std::string id = cat->Name + "##cat";
        ImGui::SetWindowPos(id.c_str(), cat->Position, ImGuiCond_Always);
    }
    for (const auto& list : categoryLists_) {
        const std::string id = "##vapelist" + list.Name;
        ImGui::SetWindowPos(id.c_str(), list.Position, ImGuiCond_Always);
    }
    const char* overlayIds[3] = { "##vapetextgui", "##vaperadar", "##vapetargetinfo" };
    for (int i = 0; i < 3; i++)
        ImGui::SetWindowPos(overlayIds[i], overlayPositions_[i], ImGuiCond_Always);
}

void VapeGui::Render() {
    // 키 입력은 GUI 가 숨겨져 있어도 처리해야 한다 (GUI 토글 바인드, 모듈 바인드).
    EnsureSettingsOptions();
    g_blurEnabled = blurBackground_;
    bool positionsClamped = false;
    if (autoRescale_) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        auto keepHeaderVisible = [&](ImVec2& pos, float width, float headerHeight) {
            if (display.x <= 0.0f || display.y <= 0.0f) return;
            const ImVec2 before = pos;
            pos.x = ImClamp(pos.x, 0.0f, ImMax(display.x - width, 0.0f));
            pos.y = ImClamp(pos.y, 0.0f, ImMax(display.y - headerHeight, 0.0f));
            if (pos.x != before.x || pos.y != before.y) positionsClamped = true;
        };
        keepHeaderVisible(MainPosition, kWinW, 41.0f);
        for (auto& cat : Categories) keepHeaderVisible(cat->Position, kWinW, 41.0f);
        for (auto& list : categoryLists_) keepHeaderVisible(list.Position, kWinW, 45.0f);
        const ImVec2 overlaySizes[3] = {
            ImVec2(210.0f, 220.0f), ImVec2(180.0f, 205.0f), ImVec2(250.0f, 105.0f)
        };
        for (int i = 0; i < 3; i++) {
            const ImVec2 before = overlayPositions_[i];
            overlayPositions_[i].x = ImClamp(
                overlayPositions_[i].x, 0.0f, ImMax(display.x - overlaySizes[i].x, 0.0f));
            overlayPositions_[i].y = ImClamp(
                overlayPositions_[i].y, 0.0f, ImMax(display.y - overlaySizes[i].y, 0.0f));
            if (overlayPositions_[i].x != before.x || overlayPositions_[i].y != before.y)
                positionsClamped = true;
        }
    }
    if (positionsClamped) ApplyStoredWindowPositions();
    ProcessKeybinds();
    if (startupNotificationPending_) {
        startupNotificationPending_ = false;
        if (guiBindIndicator_)
            CreateNotification("Finished Loading",
                               "Press " + JoinUpper(Keybind) + " to open GUI", 5.0f);
    }
    g_showTooltips = ShowTooltips;

    bool anyOptionRainbow = false;
    for (auto& cat : Categories)
        for (auto& mod : cat->Modules)
            for (auto& opt : mod->Options)
                anyOptionRainbow |= opt->Type == OptionType::ColorSlider && opt->Color.Rainbow;

    // 무지개 갱신 - GUI가 숨겨져도 color callback은 계속 진행한다.
    if (g_palette.Rainbow || anyOptionRainbow) {
        rainbowUpdateClock_ += ImGui::GetIO().DeltaTime;
        const float updateRate = ImMax(rainbowUpdateRate_->Value, 1.0f);
        if (rainbowUpdateClock_ >= 1.0f / updateRate) {
            rainbowClock_ += rainbowUpdateClock_ * 0.2f * g_palette.RainbowSpeed;
            rainbowUpdateClock_ = 0.0f;
            rainbowClock_ = std::fmod(rainbowClock_, 1.0f);
            if (g_palette.Rainbow) g_palette.Hue = rainbowClock_;
        }
    }
    for (auto& cat : Categories)
        for (auto& mod : cat->Modules)
            for (auto& opt : mod->Options)
                if (opt->Type == OptionType::ColorSlider && opt->Color.Rainbow &&
                    opt->Color.Hue != rainbowClock_) {
                    opt->Color.Hue = rainbowClock_;
                    FireOption(opt.get(), false);
                }

    RenderOverlays();
    if (!Visible) {
        RenderNotifications();
        return;
    }
    if (legitOpen_) {
        RenderLegitWindow();
        RenderNotifications();
        return;
    }

    RenderMainWindow();
    if (resetPositions_) {
        ApplyStoredWindowPositions();
        resetPositions_ = false;
    }
    for (auto& cat : Categories) RenderCategory(cat.get());
    for (int i = 0; i < 3; i++)
        if (categoryLists_[i].Visible) RenderCategoryListWindow(categoryLists_[i], i);
    RenderSearchWindow();

    // TextList / Targets 창은 카테고리 창 위에 떠야 하므로 마지막에 그린다.
    // 원본에서 이 창들은 clickgui 에 직접 붙어 있어 모듈 패널을 접어도 닫히지
    // 않는다. 따라서 행 렌더링과 무관하게 트리 전체를 훑는다.
    for (auto& cat : Categories)
        for (auto& mod : cat->Modules)
            for (auto& opt : mod->Options) {
                if (opt->Type != OptionType::TextList && opt->Type != OptionType::Targets)
                    continue;
                if (opt->WindowOpen && opt->RowSeen) {
                    if (opt->Type == OptionType::Targets) RenderTargetsWindow(opt.get());
                    else                                  RenderTextListWindow(opt.get());
                }
                // 닫힌 창은 "다음에 열릴 때 맨 앞으로" 상태로 되돌린다.
                if (!opt->WindowOpen) opt->WindowWasOpen = false;
            }

    RenderNotifications();
}

// ===========================================================================
// 저장 / 불러오기
//
// 원본은 newvape/profiles/<프로필>.txt 에 JSON 으로 저장한다. 이 이식판은 Roblox
// 쪽과 파일을 주고받지 않으므로, 줄 단위의 단순한 형식을 쓴다.
//   gui|keybind=RightShift+G
//   gui|color=0.44,1,1
//   gui|rainbow=1
//   mod|<카테고리>|<모듈>|enabled=1
//   mod|<카테고리>|<모듈>|bind=G+H
//   opt|<카테고리>|<모듈>|<옵션>=<값>
// ===========================================================================

namespace {

std::string JoinKeys(const std::vector<std::string>& keys) {
    std::string s;
    for (size_t i = 0; i < keys.size(); i++) { if (i) s += "+"; s += keys[i]; }
    return s;
}

std::vector<std::string> SplitStr(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string EncodeString(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' ') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string DecodeString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = HexDigit(value[i + 1]), lo = HexDigit(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

bool ParseVec2(const std::string& text, ImVec2& out) {
    auto parts = SplitStr(text, ',');
    float x = 0.0f, y = 0.0f;
    if (parts.size() != 2 || !ParseFiniteFloat(parts[0], x) ||
        !ParseFiniteFloat(parts[1], y))
        return false;
    out = ImVec2(x, y);
    return true;
}

// 옵션 하나를 한 줄로 직렬화한다.
std::string OptionToString(const Option* o) {
    char buf[512];
    switch (o->Type) {
    case OptionType::Toggle:
        return o->Enabled ? "1" : "0";
    case OptionType::Slider:
        std::snprintf(buf, sizeof(buf), "%.6g", o->Value);
        return buf;
    case OptionType::TwoSlider:
        std::snprintf(buf, sizeof(buf), "%.6g,%.6g", o->ValueMin, o->ValueMax);
        return buf;
    case OptionType::Dropdown:
        return EncodeString(o->Selected);
    case OptionType::TextBox:
        return EncodeString(o->Text);
    case OptionType::ColorSlider:
        std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g,%d",
                      o->Color.Hue, o->Color.Sat, o->Color.Value, o->Color.Opacity,
                      o->Color.Rainbow ? 1 : 0);
        return buf;
    case OptionType::TextList: {
        std::string a, b;
        for (size_t i = 0; i < o->ListEntries.size(); i++) {
            if (i) a += ";";
            a += EncodeString(o->ListEntries[i]);
        }
        for (size_t i = 0; i < o->ListEnabled.size(); i++) {
            if (i) b += ";";
            b += EncodeString(o->ListEnabled[i]);
        }
        return a + "|" + b;
    }
    case OptionType::Targets:
        std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d",
                      o->Players ? 1 : 0, o->NPCs ? 1 : 0,
                      o->Invisible ? 1 : 0, o->Walls ? 1 : 0);
        return buf;
    default:
        return "";
    }
}

void OptionFromString(Option* o, const std::string& v) {
    switch (o->Type) {
    case OptionType::Toggle:
        o->Enabled = (v == "1");
        break;
    case OptionType::Slider:
        {
            float value = 0.0f;
            if (ParseFiniteFloat(v, value)) {
                o->Value = ImClamp(value, o->Min, o->Max);
                o->Touched = true;
            }
        }
        break;
    case OptionType::TwoSlider: {
        auto p = SplitStr(v, ',');
        float minValue = 0.0f, maxValue = 0.0f;
        if (p.size() >= 2 && ParseFiniteFloat(p[0], minValue) &&
            ParseFiniteFloat(p[1], maxValue)) {
            minValue = ImClamp(minValue, o->Min, o->Max);
            maxValue = ImClamp(maxValue, minValue, o->Max);
            o->ValueMin = minValue;
            o->ValueMax = maxValue;
        }
        break;
    }
    case OptionType::Dropdown:
        // 원본 SetValue: 목록에 없으면 첫 항목으로
        {
            const std::string decoded = DecodeString(v);
            o->Selected = (std::find(o->List.begin(), o->List.end(), decoded) != o->List.end())
                              ? decoded : (o->List.empty() ? "None" : o->List[0]);
        }
        break;
    case OptionType::TextBox:
        o->Text = DecodeString(v);
        break;
    case OptionType::ColorSlider: {
        auto p = SplitStr(v, ',');
        float h = 0, s = 0, value = 0, opacity = 0;
        if (p.size() >= 5 && ParseFiniteFloat(p[0], h) && ParseFiniteFloat(p[1], s) &&
            ParseFiniteFloat(p[2], value) && ParseFiniteFloat(p[3], opacity)) {
            o->Color.Hue = ImClamp(h, 0.0f, 1.0f);
            o->Color.Sat = ImClamp(s, 0.0f, 1.0f);
            o->Color.Value = ImClamp(value, 0.0f, 1.0f);
            o->Color.Opacity = ImClamp(opacity, 0.0f, 1.0f);
            o->Color.Rainbow = (p[4] == "1");
        }
        break;
    }
    case OptionType::TextList: {
        size_t bar = v.find('|');
        std::string a = (bar == std::string::npos) ? v : v.substr(0, bar);
        std::string b = (bar == std::string::npos) ? "" : v.substr(bar + 1);
        o->ListEntries.clear();
        o->ListEnabled.clear();
        if (!a.empty()) for (auto& s : SplitStr(a, ';'))
            if (!s.empty()) o->ListEntries.push_back(DecodeString(s));
        if (!b.empty()) for (auto& s : SplitStr(b, ';'))
            if (!s.empty()) o->ListEnabled.push_back(DecodeString(s));
        break;
    }
    case OptionType::Targets: {
        auto p = SplitStr(v, ',');
        if (p.size() >= 4) {
            o->Players = p[0] == "1"; o->NPCs = p[1] == "1";
            o->Invisible = p[2] == "1"; o->Walls = p[3] == "1";
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

bool VapeGui::Save(const std::string& path) const {
    const std::filesystem::path target(path.empty() ? ConfigPath : path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "config|version=3\n";
    f << "gui|keybind=" << JoinKeys(Keybind) << "\n";
    f << "gui|multikeybind=" << (MultiKeybind ? 1 : 0) << "\n";
    f << "gui|tooltips=" << (ShowTooltips ? 1 : 0) << "\n";
    f << "gui|color=" << g_palette.Hue << ',' << g_palette.Sat << ',' << g_palette.Value << "\n";
    f << "gui|rainbow=" << (g_palette.Rainbow ? 1 : 0) << "\n";
    f << "gui|rainbowspeed=" << g_palette.RainbowSpeed << "\n";
    f << "gui|rainbowmode=" << g_palette.RainbowMode << "\n";
    f << "gui|mainpos=" << MainPosition.x << ',' << MainPosition.y << "\n";
    f << "gui|blur=" << (blurBackground_ ? 1 : 0) << "\n";
    f << "gui|bindindicator=" << (guiBindIndicator_ ? 1 : 0) << "\n";
    f << "gui|showlegit=" << (showLegit_ ? 1 : 0) << "\n";
    f << "gui|autorescale=" << (autoRescale_ ? 1 : 0) << "\n";
    f << "gui|teamsbyserver=" << (teamsByServer_ ? 1 : 0) << "\n";
    f << "gui|useteamcolor=" << (useTeamColor_ ? 1 : 0) << "\n";
    f << "gui|notifications=" << (notifications_ ? 1 : 0) << "\n";
    f << "gui|togglealert=" << (toggleAlert_ ? 1 : 0) << "\n";
    f << "gui|activeprofile=" << EncodeString(activeProfile_) << "\n";
    if (guiThemeDropdown_)
        f << "gui|theme=" << EncodeString(guiThemeDropdown_->Selected) << "\n";
    if (rainbowMode_)
        f << "gui|rainbowmodename=" << EncodeString(rainbowMode_->Selected) << "\n";
    if (rainbowUpdateRate_)
        f << "gui|rainbowupdaterate=" << rainbowUpdateRate_->Value << "\n";

    for (int i = 0; i < 3; i++) {
        f << "gui|overlay" << i << '=' << (overlayToggles_[i] ? 1 : 0) << "\n";
        f << "gui|overlaypos" << i << '=' << overlayPositions_[i].x << ','
          << overlayPositions_[i].y << "\n";
    }

    for (const auto& cat : Categories) {
        const std::string cn = EncodeString(cat->Name);
        f << "cat|" << cn << "|visible=" << (cat->Visible ? 1 : 0) << "\n";
        f << "cat|" << cn << "|expanded=" << (cat->Expanded ? 1 : 0) << "\n";
        f << "cat|" << cn << "|pos=" << cat->Position.x << ',' << cat->Position.y << "\n";
        for (const auto& mod : cat->Modules) {
            const std::string mn = EncodeString(mod->Name);
            f << "mod|" << cn << '|' << mn << "|enabled=" << (mod->Enabled ? 1 : 0) << "\n";
            f << "mod|" << cn << '|' << mn << "|children="
              << (mod->ChildrenVisible ? 1 : 0) << "\n";
            f << "mod|" << cn << '|' << mn << "|bind=" << JoinKeys(mod->Bind) << "\n";
            for (const auto& opt : mod->Options) {
                if (opt->Type == OptionType::Button) continue;
                f << "opt|" << cn << '|' << mn << '|' << EncodeString(opt->Name)
                  << '=' << OptionToString(opt.get()) << "\n";
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        const auto& list = categoryLists_[i];
        f << "list|" << i << "|clear=1\n";
        f << "list|" << i << "|visible=" << (list.Visible ? 1 : 0) << "\n";
        f << "list|" << i << "|expanded=" << (list.Expanded ? 1 : 0) << "\n";
        f << "list|" << i << "|settings=" << (list.SettingsVisible ? 1 : 0) << "\n";
        f << "list|" << i << "|pos=" << list.Position.x << ',' << list.Position.y << "\n";
        f << "list|" << i << "|use=" << (list.UseList ? 1 : 0) << "\n";
        f << "list|" << i << "|recolor=" << (list.RecolorVisuals ? 1 : 0) << "\n";
        f << "list|" << i << "|autosave=" << (list.AutoSave ? 1 : 0) << "\n";
        for (size_t j = 0; j < list.Entries.size(); j++)
            f << "list|" << i << "|entry=" << EncodeString(list.Entries[j]) << ','
              << (j < list.Enabled.size() && list.Enabled[j] ? 1 : 0) << "\n";
    }
    f << "config|end=1\n";
    f.flush();
    f.close();
    const bool written = (bool)f;
    if (!written) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#endif
    return true;
}

bool VapeGui::Load(const std::string& path) {
    EnsureSettingsOptions();
    std::ifstream f(path.empty() ? ConfigPath : path, std::ios::binary);
    if (!f) return false;

    std::vector<std::string> lines;
    std::string s;
    while (std::getline(f, s)) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        lines.push_back(s);
    }
    const bool version2 = !lines.empty() && lines.front() == "config|version=2";
    const bool version3 = !lines.empty() && lines.front() == "config|version=3";
    const bool legacy = !lines.empty() && lines.front().rfind("gui|", 0) == 0;
    if (f.bad() || (!version2 && !version3 && !legacy)) return false;
    if (version3 && lines.back() != "config|end=1") return false;

    auto findModule = [&](const std::string& c, const std::string& m) -> Module* {
        for (auto& cat : Categories) {
            if (cat->Name != c) continue;
            for (auto& mod : cat->Modules) if (mod->Name == m) return mod.get();
        }
        return nullptr;
    };
    auto findCategory = [&](const std::string& name) -> Category* {
        for (auto& cat : Categories) if (cat->Name == name) return cat.get();
        return nullptr;
    };
    std::unordered_map<Module*, bool> pendingEnabled;
    for (const std::string& s : lines) {
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq), val = s.substr(eq + 1);
        auto parts = SplitStr(key, '|');

        if (parts.size() == 2 && parts[0] == "gui") {
            const std::string& k = parts[1];
            if (k == "keybind") {
                auto keys = SplitStr(val, '+');
                if (!keys.empty() && !keys[0].empty()) Keybind = keys;
            } else if (k == "multikeybind")  MultiKeybind = (val == "1");
            else if (k == "tooltips")        ShowTooltips = (val == "1");
            else if (k == "rainbow")         g_palette.Rainbow = (val == "1");
            else if (k == "blur")            blurBackground_ = (val == "1");
            else if (k == "bindindicator")   guiBindIndicator_ = (val == "1");
            else if (k == "showlegit")       showLegit_ = (val == "1");
            else if (k == "autorescale")     autoRescale_ = (val == "1");
            else if (k == "teamsbyserver")   teamsByServer_ = (val == "1");
            else if (k == "useteamcolor")    useTeamColor_ = (val == "1");
            else if (k == "notifications")   notifications_ = (val == "1");
            else if (k == "togglealert")     toggleAlert_ = (val == "1");
            else if (k == "activeprofile")   activeProfile_ = DecodeString(val);
            else if (k == "theme") {
                const std::string theme = DecodeString(val);
                if (std::find(guiThemeDropdown_->List.begin(), guiThemeDropdown_->List.end(), theme) !=
                    guiThemeDropdown_->List.end()) guiThemeDropdown_->Selected = theme;
            } else if (k == "rainbowmodename") {
                const std::string mode = DecodeString(val);
                if (std::find(rainbowMode_->List.begin(), rainbowMode_->List.end(), mode) !=
                    rainbowMode_->List.end()) rainbowMode_->Selected = mode;
            } else if (k == "rainbowspeed") {
                float speed = 0.0f;
                if (ParseFiniteFloat(val, speed))
                    g_palette.RainbowSpeed = ImClamp(speed, 0.1f, 10.0f);
            } else if (k == "rainbowmode") {
                float mode = 0.0f;
                if (ParseFiniteFloat(val, mode))
                    g_palette.RainbowMode = ImClamp((int)mode, 0, 2);
            } else if (k == "rainbowupdaterate") {
                float rate = 0.0f;
                if (ParseFiniteFloat(val, rate))
                    rainbowUpdateRate_->Value = ImClamp(rate, 1.0f, 144.0f);
            } else if (k == "mainpos") {
                ParseVec2(val, MainPosition);
            } else if (k.rfind("overlaypos", 0) == 0 && k.size() == 11) {
                int index = k.back() - '0';
                if (index >= 0 && index < 3) ParseVec2(val, overlayPositions_[index]);
            } else if (k.rfind("overlay", 0) == 0 && k.size() == 8) {
                int index = k.back() - '0';
                if (index >= 0 && index < 3) overlayToggles_[index] = (val == "1");
            }
            else if (k == "color") {
                auto p = SplitStr(val, ',');
                float h = 0, sat = 0, value = 0;
                if (p.size() >= 3 && ParseFiniteFloat(p[0], h) &&
                    ParseFiniteFloat(p[1], sat) && ParseFiniteFloat(p[2], value)) {
                    g_palette.Hue = ImClamp(h, 0.0f, 1.0f);
                    g_palette.Sat = ImClamp(sat, 0.0f, 1.0f);
                    g_palette.Value = ImClamp(value, 0.0f, 1.0f);
                }
            }
        } else if (parts.size() == 3 && parts[0] == "cat") {
            if (Category* cat = findCategory(DecodeString(parts[1]))) {
                if (parts[2] == "visible") cat->Visible = (val == "1");
                else if (parts[2] == "expanded") cat->Expanded = (val == "1");
                else if (parts[2] == "pos") ParseVec2(val, cat->Position);
            }
        } else if (parts.size() == 4 && parts[0] == "mod") {
            if (Module* m = findModule(DecodeString(parts[1]), DecodeString(parts[2]))) {
                if (parts[3] == "enabled") {
                    pendingEnabled[m] = (val == "1");
                } else if (parts[3] == "children") {
                    m->ChildrenVisible = (val == "1");
                } else if (parts[3] == "bind") {
                    auto keys = SplitStr(val, '+');
                    m->Bind.clear();
                    for (auto& k : keys) if (!k.empty()) m->Bind.push_back(k);
                }
            }
        } else if (parts.size() == 4 && parts[0] == "opt") {
            if (Module* m = findModule(DecodeString(parts[1]), DecodeString(parts[2]))) {
                for (auto& opt : m->Options) {
                    if (opt->Name != DecodeString(parts[3])) continue;
                    OptionFromString(opt.get(), val);
                    FireOption(opt.get(), true);
                    break;
                }
            }
        } else if (parts.size() == 3 && parts[0] == "list") {
            int index = atoi(parts[1].c_str());
            if (index < 0 || index >= 3) continue;
            CategoryListState& list = categoryLists_[index];
            const std::string& key = parts[2];
            if (key == "clear") {
                list.Entries.clear();
                list.Enabled.clear();
            } else if (key == "visible") list.Visible = (val == "1");
            else if (key == "expanded") list.Expanded = (val == "1");
            else if (key == "settings") list.SettingsVisible = (val == "1");
            else if (key == "use") list.UseList = (val == "1");
            else if (key == "recolor") list.RecolorVisuals = (val == "1");
            else if (key == "autosave") list.AutoSave = (val == "1");
            else if (key == "pos") ParseVec2(val, list.Position);
            else if (key == "entry") {
                size_t comma = val.rfind(',');
                const std::string encoded = comma == std::string::npos ? val : val.substr(0, comma);
                list.Entries.push_back(DecodeString(encoded));
                list.Enabled.push_back(comma == std::string::npos || val.substr(comma + 1) == "1");
            }
        }
    }

    // 설정 패널의 색 슬라이더도 불러온 값에 맞춘다.
    if (categoryLists_[1].Entries.empty()) {
        categoryLists_[1].Entries = { "default" };
        categoryLists_[1].Enabled = { true };
        activeProfile_ = "default";
    }
    themeColor_->Color.Hue = g_palette.Hue;
    themeColor_->Color.Sat = g_palette.Sat;
    themeColor_->Color.Value = g_palette.Value;
    themeColor_->Color.Rainbow = g_palette.Rainbow;
    rainbowSpeed_->Value = g_palette.RainbowSpeed;
    rainbowMode_->Selected = g_palette.RainbowMode == 0 ? "Normal"
                              : (g_palette.RainbowMode == 2 ? "Retro" : "Gradient");
    rainbowClock_ = g_palette.Hue;
    if (guiThemeDropdown_->Selected == "old") {
        g_palette.Main = IM_COL32(22, 22, 23, 255);
        g_palette.Text = IM_COL32(212, 212, 212, 255);
    } else if (guiThemeDropdown_->Selected == "rise") {
        g_palette.Main = IM_COL32(31, 29, 32, 255);
        g_palette.Text = IM_COL32(232, 232, 232, 255);
    } else {
        g_palette.Main = IM_COL32(26, 25, 26, 255);
        g_palette.Text = IM_COL32(200, 200, 200, 255);
    }
    for (const auto& [module, enabled] : pendingEnabled)
        if (module->Enabled != enabled) module->Toggle();
    return true;
}

} // namespace vape
