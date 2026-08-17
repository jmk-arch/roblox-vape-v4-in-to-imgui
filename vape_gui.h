// vape_gui.h - Vape V4 ClickGUI 의 Dear ImGui 이식판
//
// 원본: newvape/guis/new.lua (6749 줄) 에서 직접 이식.
// 모든 수치(크기, 위치, 색, 트윈)는 원본 Luau 코드의 값을 그대로 사용한다.
// 추측한 값은 없다.
//
// 구조 (원본과 동일):
//
//   메인 창 (GUICategory)      220px 폭, 카테고리 버튼 목록
//     └ 카테고리 창 (Category)  220px 폭, 카테고리마다 별도 창
//         └ 모듈 버튼 (40px)
//             └ 옵션 패널 (modulechildren)
//                 └ Toggle / Slider / Dropdown / ...
//
// 사용법:
//     VapeGui gui;
//     gui.Init();
//     auto* combat = gui.CreateCategory("Combat");
//     auto* ka = combat->CreateModule("KillAura");
//     ka->CreateSlider({...});
//     ...
//     gui.Render();   // 매 프레임

#pragma once

#include "imgui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vape {

// ---------------------------------------------------------------------------
// color 모듈  (원본 new.lua 의 color.Light / color.Dark)
// ---------------------------------------------------------------------------
ImU32 Light(ImU32 base, float amount);
ImU32 Dark(ImU32 base, float amount);

// ---------------------------------------------------------------------------
// uipallet  (원본 new.lua:54-60)
//
//   Main  = Color3.fromRGB(26, 25, 26)
//   Text  = Color3.fromRGB(200, 200, 200)
//   Font  = Arial
//   Tween = TweenInfo.new(0.16, Enum.EasingStyle.Linear)
// ---------------------------------------------------------------------------
struct Palette {
    ImU32 Main = IM_COL32(26, 25, 26, 255);
    ImU32 Text = IM_COL32(200, 200, 200, 255);
    float Tween = 0.16f;                       // 초, Linear

    // mainapi.GUIColor - GUI 강조색 (HSV 로 보관)
    float Hue = 0.44f;
    float Sat = 1.0f;
    float Value = 1.0f;
    bool  Rainbow = false;
    float RainbowSpeed = 1.0f;
    int   RainbowMode = 1;                    // 0=Normal, 1=Gradient, 2=Retro

    ImU32 Accent() const;
    // 인덱스별 무지개 오프셋: (hue - index * step) % 1
    // 옵션은 step 0.075, 모듈 버튼은 0.025 를 쓴다 (원본 그대로).
    ImU32 AccentStep(int index, float step) const;
    ImU32 AccentIndexed(int index) const { return AccentStep(index, 0.075f); }
};

extern Palette g_palette;

// 원본은 요소마다 TextSize 가 다르다 (11 / 12 / 13 / 14 / 15).
// 앱 초기화에서 Arial 을 각 크기로 올려 여기에 넣어주면 그대로 반영된다.
// nullptr 이면 기본 폰트를 쓴다. 14 는 io.Fonts 의 첫 폰트(기본)를 그대로 쓴다.
extern ImFont* g_fontSmall;   // TextSize 11 (슬라이더 라벨/값, TextList items)
extern ImFont* g_fontBox;     // TextSize 12 (TextBox 제목/입력)
extern ImFont* g_fontTitle;   // TextSize 13 (창 제목, Dropdown)
extern ImFont* g_fontItem;    // TextSize 15 (TextList/Targets 제목, 창 입력)
extern ImFont* g_fontSemiBold;// TextSize 14 SemiBold (알림 제목)

enum class NotificationType {
    Info,
    Warning,
    Alert,
};

// ---------------------------------------------------------------------------
// 옵션 값
// ---------------------------------------------------------------------------
struct ColorValue {
    float Hue = 0.44f;
    float Sat = 1.0f;
    float Value = 1.0f;
    float Opacity = 1.0f;
    bool  Rainbow = false;
};

// ---------------------------------------------------------------------------
// 옵션 (components.* 에 대응)
// ---------------------------------------------------------------------------
enum class OptionType {
    Toggle, Slider, TwoSlider, Dropdown, TextBox,
    ColorSlider, Button, TextList, Targets,
};

struct Option {
    OptionType  Type = OptionType::Toggle;
    std::string Name;
    std::string Tooltip;
    bool        Visible = true;
    bool        Darker = false;     // optionsettings.Darker
    int         Index = 0;          // 무지개 오프셋용

    // Toggle
    bool Enabled = false;

    // Slider / TwoSlider
    float Value = 0.0f;
    float ValueMin = 0.0f;
    float ValueMax = 10.0f;
    float Min = 0.0f;
    float Max = 100.0f;
    float Decimal = 1.0f;           // optionsettings.Decimal
    std::string Suffix;

    // Dropdown
    std::vector<std::string> List;
    std::string              Selected;

    // TextBox
    std::string Text;
    std::string Placeholder;

    // ColorSlider
    ColorValue Color;
    bool       Expanded = false;    // 확장 서브 슬라이더

    // TextList
    std::vector<std::string> ListEntries;
    std::vector<std::string> ListEnabled;
    char                     AddBuffer[128] = {};
    bool                     WindowOpen = false;
    // optionsettings.Color - 기본 Color3.fromRGB(5, 134, 105)
    ImU32                    ListColor = IM_COL32(5, 134, 105, 255);

    // Targets
    bool Players = false, NPCs = false, Invisible = false, Walls = false;
    // Targets 창 안의 Toggle 두 개 (원본은 components.Toggle 을 그대로 쓴다)
    bool InvisibleHover = false, WallsHover = false;

    std::function<void()> Function;
    // 모든 옵션 타입에서 값과 commit 여부를 받을 수 있는 공통 콜백.
    std::function<void(const Option&, bool)> Changed;

    Option* OnChange(std::function<void(const Option&, bool)> fn) {
        Changed = std::move(fn);
        return this;
    }

    // 내부 상태
    bool  BoxEditing = false;
    bool  BoxEditing2 = false;
    char  EditBuffer[64] = {};
    float RainbowLit = 0.0f;
    // 원본 Slider 는 생성 시 fill 을 (Value - Min) / Max 로, SetValue 이후에는
    // Value / Max 로 계산한다. 이 차이를 그대로 재현하기 위한 플래그.
    bool  Touched = false;
    bool  InteractionActive = false;
    // TextList / Targets 창 위치 = 행의 화면 좌표 + (220, 0).
    // 원본에서 창 위치는 행의 AbsolutePosition 을 따라가므로, 행이 그려진 적이
    // 있어야(RowSeen) 창을 띄운다.
    ImVec2 RowScreenPos = ImVec2(0, 0);
    bool   RowSeen = false;
    bool   WindowWasOpen = false;
};

// ---------------------------------------------------------------------------
// 모듈  (categoryapi:CreateModule)
// ---------------------------------------------------------------------------
struct Module {
    std::string Name;
    std::string Tooltip;
    std::string ExtraText;
    bool        Enabled = false;
    bool        ChildrenVisible = false;   // modulechildren.Visible
    std::vector<std::string> Bind;
    int         Index = 0;

    std::vector<std::unique_ptr<Option>> Options;

    // bind 상태 문구. 배경 이미지는 쓰지 않고 모듈 행 위에 직접 표시한다.
    std::string BindCoverText;
    float       BindCoverTimer = 0.0f;

    std::function<void(bool)> Function;

    Option* CreateToggle(const std::string& name, bool def = false,
                         const std::string& tooltip = "");
    Option* CreateSlider(const std::string& name, float min, float max,
                         float def, float decimal = 1.0f,
                         const std::string& suffix = "",
                         const std::string& tooltip = "");
    Option* CreateTwoSlider(const std::string& name, float min, float max,
                            float defMin, float defMax, float decimal = 1.0f,
                            const std::string& tooltip = "");
    Option* CreateDropdown(const std::string& name,
                           const std::vector<std::string>& list,
                           const std::string& tooltip = "");
    Option* CreateTextBox(const std::string& name,
                          const std::string& placeholder = "Click to set",
                          const std::string& tooltip = "");
    Option* CreateColorSlider(const std::string& name,
                              const std::string& tooltip = "");
    Option* CreateButton(const std::string& name,
                         std::function<void()> fn,
                         const std::string& tooltip = "");
    Option* CreateTextList(const std::string& name,
                           const std::string& tooltip = "");
    Option* CreateTargets(const std::string& tooltip = "");

    void Toggle();
};

// ---------------------------------------------------------------------------
// 카테고리  (mainapi:CreateCategory)
// ---------------------------------------------------------------------------
struct Category {
    std::string Name;
    bool        Expanded = false;
    bool        Visible = false;       // 창 표시 여부
    ImVec2      Position = ImVec2(236, 60);
    int         IconIndex = 0;

    std::vector<std::unique_ptr<Module>> Modules;

    Module* CreateModule(const std::string& name,
                         std::function<void(bool)> fn = nullptr,
                         const std::string& tooltip = "");
    void Expand();
};

// ---------------------------------------------------------------------------
// 메인 API
// ---------------------------------------------------------------------------
class VapeGui {
public:
    void Init();
    void Render();
    void OpenSettings();
    bool OpenCategory(const std::string& name, bool expand = false);
    bool IsBinding() const { return BindingModule != nullptr || BindingGui; }
    void CreateNotification(const std::string& title, const std::string& text,
                            float duration = 5.0f,
                            NotificationType type = NotificationType::Info);
    Category* CreateCategory(const std::string& name);

    bool Visible = true;               // clickgui.Visible
    ImVec2 MainPosition = ImVec2(6, 60);   // window.Position (원본과 동일)

    std::vector<std::unique_ptr<Category>> Categories;

    // -----------------------------------------------------------------------
    // 키바인드  (원본 gui.lua:5052 InputBegan / 5093 InputEnded)
    // -----------------------------------------------------------------------
    std::vector<std::string> Keybind{ "RightShift" };   // mainapi.Keybind - GUI 토글
    std::vector<std::string> HeldKeys;                  // mainapi.HeldKeybinds
    Module* BindingModule = nullptr;                    // mainapi.Binding (모듈)
    bool    BindingGui = false;                         // mainapi.Binding (GUI 바인드)
    bool    MultiKeybind = false;                       // 여러 키 조합 허용
    bool    ShowTooltips = true;

    // -----------------------------------------------------------------------
    // 설정 저장/불러오기
    // -----------------------------------------------------------------------
    // 성공 여부를 돌려준다. 경로가 비어 있으면 기본값(vape_config.txt)을 쓴다.
    bool Save(const std::string& path = "") const;
    bool Load(const std::string& path = "");
    std::string ConfigPath = "vape_config.txt";

    // 앱 셸이 처리하는 수명주기 요청.
    bool ExitRequested = false;
    bool RestartRequested = false;
    bool SkipSaveOnExit = false;

private:
    struct CategoryListState {
        std::string Name;
        bool Visible = false;
        bool Expanded = false;
        bool SettingsVisible = false;
        ImVec2 Position = ImVec2(240, 46);
        std::vector<std::string> Entries;
        std::vector<bool> Enabled;
        bool UseList = true;
        bool RecolorVisuals = true;
        bool AutoSave = true;
        char AddBuffer[128] = {};
    };

    struct Toast {
        std::string Title;
        std::string Text;
        float Duration = 0.0f;
        float Age = 0.0f;
        float StackOffset = 0.0f;
        float StackFrom = 0.0f;
        float StackTarget = 0.0f;
        float StackTweenAge = 0.4f;
        NotificationType Type = NotificationType::Info;
    };

    void RenderMainWindow();
    void RenderSearchWindow();
    void RenderLegitWindow();
    void RenderOverlays();
    void RenderNotifications();
    void RenderCategoryListWindow(CategoryListState& list, int index);
    void RenderCategory(Category* cat);
    void RenderModule(Category* cat, Module* mod, float& y);
    // childrenBg 는 원본 children.BackgroundColor3 (옵션 배경 계산의 기준색).
    void RenderOption(Option* opt, float width, float& y, bool darker, ImU32 childrenBg);
    // TextList / Targets 의 별도 창 (원본은 clickgui 에 직접 붙는다)
    void RenderTextListWindow(Option* opt);
    void RenderTargetsWindow(Option* opt);
    // settingspane - 메인 창을 덮는 설정 화면
    void RenderSettingsPane(ImVec2 wp, float w, float h);
    // 눌린 키를 모아 GUI/모듈 바인드를 처리한다 (원본 InputBegan/InputEnded)
    void ProcessKeybinds();
    void BeginModuleBinding(Module* mod);
    void BeginGuiBinding();
    void FinishBinding(const std::vector<std::string>& keys);
    void CancelBinding();
    void ToggleModule(Module* mod, bool notify = false);
    void EnsureSettingsOptions();
    void ApplyStoredWindowPositions();

    bool  settingsOpen_ = false;
    int   settingsPage_ = -1;
    bool  legitOpen_ = false;
    bool  overlaysOpen_ = false;
    bool  overlayToggles_[3] = {};
    ImVec2 overlayPositions_[3] = {
        ImVec2(760, 40), ImVec2(760, 220), ImVec2(760, 400)
    };
    bool  blurBackground_ = true;
    bool  guiBindIndicator_ = true;
    bool  showLegit_ = true;
    bool  autoRescale_ = true;
    bool  teamsByServer_ = true;
    bool  useTeamColor_ = true;
    bool  notifications_ = true;
    bool  toggleAlert_ = true;
    std::string activeProfile_ = "default";
    char  searchBuffer_[128] = {};
    CategoryListState categoryLists_[3];
    float rainbowClock_ = 0.0f;
    float rainbowUpdateClock_ = 0.0f;
    bool  resetPositions_ = false;   // 다음 렌더에서 저장된 위치를 강제로 적용
    bool  startupNotificationPending_ = true;
    // 설정 패널 내용 높이. 원본은 카테고리가 10개라 창이 충분히 길지만, 이 이식판은
    // 카테고리 수에 따라 창이 짧을 수 있으므로 설정을 여는 동안 창을 늘려준다.
    float settingsHeight_ = 320.0f;

    // 설정 패널이 쓰는 옵션 컴포넌트 (원본 GUIColor / RainbowSpeed 에 대응)
    std::unique_ptr<Option> themeColor_;
    std::unique_ptr<Option> rainbowSpeed_;
    std::unique_ptr<Option> guiThemeDropdown_;
    std::unique_ptr<Option> rainbowMode_;
    std::unique_ptr<Option> rainbowUpdateRate_;
    std::vector<unsigned char> previousKeyDown_;
    std::vector<std::string> bindingKeys_;
    std::string bindingLastKey_;
    bool keyStatesInitialized_ = false;
    std::vector<Toast> toasts_;
};

} // namespace vape
