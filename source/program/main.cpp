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

    /*
     * チームカラー表 (main 1.0 系 BuildID 28c4287a 専用のオフセット)
     *
     *   *(main + 0x6056160)            = チームカラー管理オブジェクト
     *   obj + 0x2b8 + team * 0x12c     = チームごとの色ブロック
     *   block + kind * 0x10            = sead::Color4f (kind は 0..16 の色バリエーション)
     *
     * 根拠: シェーダーの my_team_color 系パラメータを設定する処理 (0x1344b00) が、
     *       色の取得関数 (0x1345e58) を通してこの表を読んでいる。
     */
    namespace teamcolor {
        constexpr uintptr_t InstanceOffset = 0x6056160;
        constexpr uintptr_t TableOffset    = 0x2b8;
        constexpr size_t    TeamStride     = 0x12c;
        constexpr int       KindCount      = 17;
        constexpr int       TeamCount      = 2; /* 0 = Alpha, 1 = Bravo */

        struct Color4f { float r, g, b, a; };

        Color4f* GetTeamBlock(int team) {
            uintptr_t instance = *reinterpret_cast<uintptr_t*>(exl::util::modules::GetTargetOffset(InstanceOffset));
            if (instance == 0) return nullptr;
            return reinterpret_cast<Color4f*>(instance + TableOffset + team * TeamStride);
        }
    }

    struct TeamOverride {
        const char* name;
        bool  enabled  = false;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        /* 有効にした時点の元の色(無効に戻すときに使う) */
        bool  hasBackup = false;
        teamcolor::Color4f backup[teamcolor::KindCount] = {};
    };

    TeamOverride g_Teams[teamcolor::TeamCount] = {
        { "Alpha", false, { 1.00f, 0.35f, 0.00f } },
        { "Bravo", false, { 0.10f, 0.40f, 1.00f } },
    };

    /* 毎フレーム呼ぶ。有効なチームの色を書き換え、無効にしたら元に戻す。 */
    void ApplyTeamColors() {
        for (int team = 0; team < teamcolor::TeamCount; team++) {
            TeamOverride& o = g_Teams[team];
            teamcolor::Color4f* block = teamcolor::GetTeamBlock(team);
            if (block == nullptr) continue;

            if (o.enabled) {
                if (!o.hasBackup) {
                    for (int k = 0; k < teamcolor::KindCount; k++) o.backup[k] = block[k];
                    o.hasBackup = true;
                }
                for (int k = 0; k < teamcolor::KindCount; k++) {
                    block[k].r = o.color[0];
                    block[k].g = o.color[1];
                    block[k].b = o.color[2];
                    /* アルファ(不透明度)は元のまま */
                }
            } else if (o.hasBackup) {
                /* ゲームがまだ書き換えていなければ(自分の色のままなら)元に戻す */
                if (block[0].r == o.color[0] && block[0].g == o.color[1] && block[0].b == o.color[2]) {
                    for (int k = 0; k < teamcolor::KindCount; k++) block[k] = o.backup[k];
                }
                o.hasBackup = false;
            }
        }
    }

    void DrawTeamRow(int team) {
        TeamOverride& o = g_Teams[team];
        ImGui::PushID(team);

        teamcolor::Color4f* block = teamcolor::GetTeamBlock(team);
        ImVec4 current = block ? ImVec4(block[0].r, block[0].g, block[0].b, 1.0f) : ImVec4(0, 0, 0, 1);

        ImGui::Text("%s team", o.name);
        ImGui::SameLine();
        ImGui::ColorButton("##current", current, ImGuiColorEditFlags_NoTooltip, ImVec2(22, 22));
        ImGui::SameLine();
        ImGui::TextDisabled("now");

        ImGui::Checkbox("Override", &o.enabled);
        ImGui::ColorEdit3("RGB", o.color,
                          ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB);

        ImGui::PopID();
    }

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

    void OnNewFrame() {
        ApplyTeamColors();
    }

    void OnRender() {
        ImGui::SetNextWindowPos(ImVec2(32, 32), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Chroma Rush")) {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.75f, 1.0f), "L3 + R3 : toggle controller input");
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Ink", ImGuiTreeNodeFlags_DefaultOpen)) {
                DrawTeamRow(0);
                ImGui::Separator();
                DrawTeamRow(1);
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
