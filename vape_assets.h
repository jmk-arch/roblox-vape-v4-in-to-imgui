// vape_assets.h - 원본 PNG 에셋 로더
//
// newvape/assets/new/ 의 PNG 를 GPU 텍스처로 올려서 원본과 픽셀 단위로 동일한
// 아이콘을 그린다. 로드에 실패하면 각 아이콘은 벡터 폴백으로 그려진다.

#pragma once

#include "imgui.h"

#include <string>

namespace vape {

// 원본 에셋 이름 (newvape/assets/new/<name>.png)
enum class Asset {
    Add, Alert, AllowedIcon, AllowedTab, ArrowModule, Back, Bind, BindBkg,
    BlatantIcon, BlockedIcon, BlockedTab, Blur, BlurNotif, Close, CloseMini,
    ColorPreview, CombatIcon, CustomSettings, Discord, Dots, Edit,
    ExpandIcon, ExpandRight, ExpandUp, FriendsTab, GuiSettings, GuiSlider,
    GuiSliderRain, GuiV4, GuiVape, Info, InventoryIcon, Legit, LegitTab,
    MiniIcon, Notification, OverlaysIcon, OverlaysTab, Pin, ProfilesIcon,
    RadarIcon, Rainbow1, Rainbow2, Rainbow3, Rainbow4, Range, RangeArrow,
    RenderIcon, RenderTab, Search, TargetInfoIcon, TargetNpc1, TargetNpc2,
    TargetPlayers1, TargetPlayers2, TargetsTab, TextGuiIcon, TextV4,
    TextVape, UtilityIcon, Vape, Warning, WorldIcon,
    COUNT
};

struct AssetTexture {
    ImTextureID ID = (ImTextureID)0;
    int Width = 0;
    int Height = 0;
    bool Valid = false;
};

// 백엔드가 D3D11 이면 device 를, 아니면 nullptr 을 넘긴다.
// assetDir 은 PNG 가 들어있는 폴더 (예: "assets").
// 반환값: 성공적으로 로드된 텍스처 수.
int LoadAssets(void* d3dDevice, const std::string& assetDir);
void UnloadAssets();

// 로드된 텍스처 조회. 실패했으면 Valid == false.
const AssetTexture& GetAsset(Asset a);

// 에셋이 하나라도 로드됐는지 (아이콘을 텍스처로 그릴지 벡터로 그릴지 판단)
bool AssetsLoaded();

// 텍스처를 지정 사각형에 그린다 (tint 적용). 로드 실패 시 false 반환.
bool DrawAsset(ImDrawList* dl, Asset a, ImVec2 min, ImVec2 max, ImU32 tint);
// 중심 + 크기 지정 버전
bool DrawAssetSized(ImDrawList* dl, Asset a, ImVec2 topLeft, float w, float h, ImU32 tint);
// 회전 그리기 (Rotation 속성 대응, 도 단위)
bool DrawAssetRotated(ImDrawList* dl, Asset a, ImVec2 center, float w, float h,
                      float rotDeg, ImU32 tint);

// 9-slice 그리기 (Enum.ScaleType.Slice + SliceCenter 대응).
// src* 는 원본 텍스처 픽셀 기준의 잘라낼 영역, border 는 늘어나지 않는 테두리 폭.
bool DrawAssetSliced(ImDrawList* dl, Asset a, ImVec2 min, ImVec2 max,
                     float srcX, float srcY, float srcW, float srcH,
                     float borderL, float borderT, float borderR, float borderB,
                     ImU32 tint, float destinationScale = 1.0f);

} // namespace vape
