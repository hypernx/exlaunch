#include "lib.hpp"

#include <imgui.h>
#include <imgui_xeno.h>

/*
 * Chroma Rush
 *
 * NVN の nvnBootstrapLoader をフックして imgui-xeno に渡し、
 * ゲームの画面表示の直前に ImGui のメニューを重ね描きする。
 * 操作: 左右のスティックを同時に押し込む (L3 + R3) と、コントローラーでメニューを操作できる。
 */

namespace chroma {

    /* メニューで変更する設定値。ゲームへの反映は未実装。 */
    struct Settings {
        bool  customInk   = false;
        float inkColor[3] = { 1.0f, 0.35f, 0.0f };
    };

    Settings g_Settings;

    void ApplyStyle() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 8.0f;
        style.FrameRounding     = 6.0f;
        style.GrabRounding      = 6.0f;
        style.WindowBorderSize  = 0.0f;
        style.WindowPadding     = ImVec2(14, 12);
        style.ItemSpacing       = ImVec2(10, 8);

        ImVec4* c = style.Colors;
        const ImVec4 accent      = ImVec4(0.93f, 0.25f, 0.70f, 1.00f); /* ピンク */
        const ImVec4 accentHover = ImVec4(1.00f, 0.42f, 0.80f, 1.00f);
        const ImVec4 sub         = ImVec4(0.20f, 0.85f, 0.75f, 1.00f); /* ミント */

        c[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.06f, 0.10f, 0.92f);
        c[ImGuiCol_TitleBg]          = ImVec4(0.12f, 0.08f, 0.16f, 1.00f);
        c[ImGuiCol_TitleBgActive]    = ImVec4(0.30f, 0.07f, 0.24f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.08f, 0.16f, 0.80f);
        c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.13f, 0.22f, 1.00f);
        c[ImGuiCol_FrameBgHovered]   = ImVec4(0.26f, 0.15f, 0.30f, 1.00f);
        c[ImGuiCol_FrameBgActive]    = ImVec4(0.34f, 0.16f, 0.36f, 1.00f);
        c[ImGuiCol_CheckMark]        = sub;
        c[ImGuiCol_SliderGrab]       = accent;
        c[ImGuiCol_SliderGrabActive] = accentHover;
        c[ImGuiCol_Button]           = ImVec4(0.45f, 0.10f, 0.35f, 1.00f);
        c[ImGuiCol_ButtonHovered]    = accent;
        c[ImGuiCol_ButtonActive]     = accentHover;
        c[ImGuiCol_Header]           = ImVec4(0.45f, 0.10f, 0.35f, 1.00f);
        c[ImGuiCol_HeaderHovered]    = accent;
        c[ImGuiCol_HeaderActive]     = accentHover;
        c[ImGuiCol_Separator]        = ImVec4(0.93f, 0.25f, 0.70f, 0.40f);
        c[ImGuiCol_NavHighlight]     = sub;
    }

    void OnPreInit() {}

    void OnPostInit() {
        ImGui::GetIO().FontGlobalScale = 1.4f;
        ApplyStyle();
    }

    void OnNewFrame() {}

    void OnRender() {
        ImGui::SetNextWindowPos(ImVec2(32, 32), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Chroma Rush")) {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.75f, 1.0f), "L3 + R3 : toggle controller input");
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Ink", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Custom ink color", &g_Settings.customInk);
                ImGui::ColorEdit3("Color", g_Settings.inkColor);
                ImGui::TextDisabled("(not applied to the game yet)");
            }
        }
        ImGui::End();
    }

}

/* nvnBootstrapLoader は sdk から公開されている。 */
extern "C" void* nvnBootstrapLoader(const char* name);

HOOK_DEFINE_TRAMPOLINE(NvnBootstrapHook) {
    static void* CallOrig(const char* name) {
        return Orig(name);
    }

    static void* Callback(const char* name) {
        return imgui_xeno_bootstrap_hook(name, &CallOrig);
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    /* Setup hooking environment. */
    exl::hook::Initialize();

    imgui_xeno_add_on_pre_init(&chroma::OnPreInit);
    imgui_xeno_add_on_new_frame(&chroma::OnNewFrame);
    imgui_xeno_init(&chroma::OnPostInit, &chroma::OnRender);

    NvnBootstrapHook::InstallAtFuncPtr(nvnBootstrapLoader);
}

extern "C" NORETURN void exl_exception_entry() {
    /* Note: this is only applicable in the context of applets/sysmodules. */
    EXL_ABORT("Default exception handler called!");
}
