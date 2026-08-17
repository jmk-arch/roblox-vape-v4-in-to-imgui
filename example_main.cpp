// example_main.cpp - 사용 예시
//
// 원본 Luau 쪽 등록 방식과 1:1 대응:
//
//   local Combat = vape:CreateCategory({Name = 'Combat', ...})
//   local KillAura = Combat:CreateModule({Name = 'KillAura', Function = ...})
//   KillAura:CreateSlider({Name = 'Range', Min = 1, Max = 30, Default = 18})

#include "vape_gui.h"

#include <cstdio>

namespace {
void ShowValue(vape::Module* module, float value, const char* suffix = "") {
    char text[48];
    if (suffix && *suffix)
        std::snprintf(text, sizeof(text), "%.1f %s", value, suffix);
    else
        std::snprintf(text, sizeof(text), "%.1f", value);
    module->ExtraText = text;
}
}

void SetupVapeGui(vape::VapeGui& gui) {
    gui.Init();

    // mainapi.GUIColor - 기본 강조색
    vape::g_palette.Hue = 0.44f;
    vape::g_palette.Sat = 1.0f;
    vape::g_palette.Value = 1.0f;

    // 원본 main GUI 순서: Combat, Blatant, Render, Utility, World,
    // Inventory, Minigames. Friends/Profiles/Targets 는 VapeGui 가 별도
    // CategoryList 로 렌더링한다.
    auto* combat = gui.CreateCategory("Combat");
    auto* blatant = gui.CreateCategory("Blatant");
    (void)blatant;

    auto* killaura = combat->CreateModule("KillAura");
    auto* range = killaura->CreateSlider("Range", 1.0f, 30.0f, 18.0f, 10.0f, "studs",
                                         "Maximum attack distance");
    range->OnChange([killaura](const vape::Option& value, bool) {
        ShowValue(killaura, value.Value, "studs");
    });
    ShowValue(killaura, range->Value, "studs");
    killaura->CreateTwoSlider("Delay", 0.0f, 1000.0f, 0.0f, 100.0f, 1.0f);
    killaura->CreateDropdown("Sort", { "Distance", "Health", "Angle" });
    killaura->CreateToggle("Auto block", true, "Block while attacking");
    killaura->CreateToggle("Raycast");
    killaura->CreateTargets();

    auto* reach = combat->CreateModule("Reach");
    auto* reachAmount = reach->CreateSlider("Amount", 1.0f, 6.0f, 3.0f, 10.0f, "studs");
    reachAmount->OnChange([reach](const vape::Option& value, bool) {
        ShowValue(reach, value.Value, "studs");
    });
    ShowValue(reach, reachAmount->Value, "studs");

    auto* autoclicker = combat->CreateModule("AutoClicker");
    auto* cps = autoclicker->CreateSlider("CPS", 1.0f, 20.0f, 12.0f, 1.0f);
    cps->OnChange([autoclicker](const vape::Option& value, bool) {
        ShowValue(autoclicker, value.Value, "cps");
    });
    ShowValue(autoclicker, cps->Value, "cps");

    auto* render = gui.CreateCategory("Render");

    auto* esp = render->CreateModule("ChestESP");
    esp->CreateColorSlider("Color");
    esp->CreateDropdown("Mode", { "Box", "Outline", "Fill" });
    esp->CreateTextList("Blacklist", "Entries to ignore");

    auto* nametags = render->CreateModule("Nametags");
    nametags->CreateSlider("Size", 1.0f, 30.0f, 14.0f, 1.0f);
    nametags->CreateToggle("Show health", true);

    auto* utility = gui.CreateCategory("Utility");

    auto* chat = utility->CreateModule("ChatPrefix");
    auto* prefix = chat->CreateTextBox("Prefix", "Type a prefix");
    prefix->Text = "-";
    prefix->OnChange([chat](const vape::Option& value, bool) {
        chat->ExtraText = value.Text;
    });
    chat->ExtraText = prefix->Text;
    chat->CreateButton("Reset", [prefix, chat] {
        prefix->Text = "-";
        chat->ExtraText = prefix->Text;
    });

    utility->CreateModule("Sprint");
    utility->CreateModule("AutoTool");

    auto* world = gui.CreateCategory("World");
    auto* scaffold = world->CreateModule("Scaffold");
    scaffold->CreateSlider("Blocks", 1.0f, 10.0f, 3.0f, 1.0f);
    scaffold->CreateToggle("Tower");

    gui.CreateCategory("Inventory");
    gui.CreateCategory("Minigames");
}

// 매 프레임 NewFrame() 과 Render() 사이에서 호출
void RenderVapeGui(vape::VapeGui& gui) {
    gui.Render();
}
