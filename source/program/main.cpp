#include "lib.hpp"

#include <cmath>
#include <cstdio>
#include <cfloat>

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

    /* ------------------------------------------------------------------ */
    /*  見た目                                                             */
    /* ------------------------------------------------------------------ */

    namespace pal {
        /* ネオン系のポップな配色 */
        constexpr ImU32 Pink   = IM_COL32(255,  62, 165, 255);
        constexpr ImU32 Lime   = IM_COL32(198, 255,   0, 255);
        constexpr ImU32 Cyan   = IM_COL32( 25, 211, 243, 255);
        constexpr ImU32 Purple = IM_COL32(123,  47, 255, 255);
        constexpr ImU32 Orange = IM_COL32(255, 138,   0, 255);
        constexpr ImU32 Ink    = IM_COL32( 22,  14,  38, 255);
        constexpr ImU32 White  = IM_COL32(255, 255, 255, 255);

        constexpr ImU32 Cycle[] = { Pink, Lime, Cyan, Orange, Purple };
        constexpr int CycleCount = sizeof(Cycle) / sizeof(Cycle[0]);

        ImU32 WithAlpha(ImU32 c, float a) {
            int ai = (int) (a * 255.0f);
            if (ai < 0) ai = 0;
            if (ai > 255) ai = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32) ai << IM_COL32_A_SHIFT);
        }
    }

    float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    /* イージング: ぽよんと跳ねる */
    float EaseOutBack(float t) {
        t = Clamp01(t);
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }

    /* インクのしぶき(大きな円 + まわりの小さな粒) */
    void DrawSplat(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int seed) {
        dl->AddCircleFilled(c, r, col, 32);
        for (int i = 0; i < 7; i++) {
            float ang  = (float) (seed * 37 + i * 53) * 0.1745f;
            float dist = r * (1.05f + 0.35f * (float) ((seed + i * 7) % 5) / 4.0f);
            float rr   = r * (0.14f + 0.10f * (float) ((seed * 3 + i) % 4) / 3.0f);
            dl->AddCircleFilled(ImVec2(c.x + cosf(ang) * dist, c.y + sinf(ang) * dist), rr, col, 16);
        }
    }

    /* 文字ごとに色を変えて、波のように上下させたタイトル */
    void DrawBouncyTitle(ImDrawList* dl, const char* text, ImVec2 center, float size, float time, float alpha) {
        ImFont* font = ImGui::GetFont();
        ImVec2 total = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        float x = center.x - total.x * 0.5f;
        int colorIndex = 0;
        for (const char* p = text; *p; p++) {
            char ch[2] = { *p, 0 };
            ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ch);
            if (*p != ' ') {
                float bob = sinf(time * 6.0f + (float) (p - text) * 0.55f) * size * 0.08f;
                ImVec2 pos(x, center.y - total.y * 0.5f + bob);
                ImU32 col = pal::Cycle[colorIndex++ % pal::CycleCount];
                /* 影 → 本体 */
                dl->AddText(font, size, ImVec2(pos.x + size * 0.06f, pos.y + size * 0.06f), pal::WithAlpha(pal::Ink, alpha), ch);
                dl->AddText(font, size, pos, pal::WithAlpha(col, alpha), ch);
            }
            x += sz.x;
        }
    }

    /* ------------------------------------------------------------------ */
    /*  ロード画面                                                         */
    /* ------------------------------------------------------------------ */

    namespace intro {
        constexpr float Duration = 3.2f;   /* ロードの長さ(秒) */
        constexpr float FadeOut  = 0.45f;  /* 消えるまで(秒) */

        float g_Time   = 0.0f;
        bool  g_Active = true;

        void Restart() { g_Time = 0.0f; g_Active = true; }

        /* true の間はロード画面を表示中 */
        bool Draw() {
            if (!g_Active) return false;

            ImGuiIO& io = ImGui::GetIO();
            g_Time += io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : (1.0f / 60.0f);
            float t = g_Time;
            if (t >= Duration + FadeOut) {
                g_Active = false;
                return false;
            }

            float fade = t > Duration ? 1.0f - (t - Duration) / FadeOut : 1.0f;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 ds = io.DisplaySize;
            ImVec2 mid(ds.x * 0.5f, ds.y * 0.5f);
            float unit = ds.y / 720.0f;

            /* 背景: 濃い紫のグラデーション */
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ds,
                                        pal::WithAlpha(IM_COL32(40, 10, 70, 255), 0.92f * fade),
                                        pal::WithAlpha(IM_COL32(15, 30, 80, 255), 0.92f * fade),
                                        pal::WithAlpha(IM_COL32(60, 10, 60, 255), 0.92f * fade),
                                        pal::WithAlpha(IM_COL32(10, 10, 30, 255), 0.92f * fade));

            /* 順番に広がるインクのしぶき */
            struct SplatDef { float x, y, r, at; int color; };
            static const SplatDef splats[] = {
                { 0.14f, 0.22f, 70, 0.05f, 0 }, { 0.86f, 0.18f, 60, 0.25f, 2 },
                { 0.10f, 0.80f, 85, 0.45f, 1 }, { 0.88f, 0.78f, 75, 0.65f, 3 },
                { 0.32f, 0.10f, 38, 0.85f, 4 }, { 0.70f, 0.90f, 45, 1.00f, 0 },
                { 0.50f, 0.85f, 30, 1.20f, 2 }, { 0.62f, 0.12f, 32, 1.35f, 1 },
            };
            for (int i = 0; i < (int) (sizeof(splats) / sizeof(splats[0])); i++) {
                const SplatDef& s = splats[i];
                float k = EaseOutBack((t - s.at) / 0.5f);
                if (k <= 0.0f) continue;
                DrawSplat(dl, ImVec2(ds.x * s.x, ds.y * s.y), s.r * unit * k,
                          pal::WithAlpha(pal::Cycle[s.color], 0.85f * fade), i + 1);
            }

            /* タイトル: ぽよんと出てきて、文字が波打つ */
            float pop = EaseOutBack((t - 0.2f) / 0.7f);
            if (pop > 0.0f) {
                DrawBouncyTitle(dl, "CHROMA RUSH", ImVec2(mid.x, mid.y - 40 * unit), 84.0f * unit * pop, t, fade);
            }

            /* サブタイトル */
            float subA = Clamp01((t - 0.9f) / 0.4f) * fade;
            if (subA > 0.0f) {
                const char* sub = "~ splash your own colors ~";
                float sz = 22.0f * unit;
                ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(sz, FLT_MAX, 0.0f, sub);
                dl->AddText(ImGui::GetFont(), sz, ImVec2(mid.x - tsz.x * 0.5f, mid.y + 20 * unit),
                            pal::WithAlpha(pal::White, subA), sub);
            }

            /* ローディングバー: インクが流れ込むように伸びる */
            float barW = 420.0f * unit, barH = 22.0f * unit;
            ImVec2 b0(mid.x - barW * 0.5f, mid.y + 80 * unit), b1(mid.x + barW * 0.5f, b0.y + barH);
            float prog = Clamp01(t / (Duration - 0.3f));
            prog = 1.0f - (1.0f - prog) * (1.0f - prog);
            dl->AddRectFilled(b0, b1, pal::WithAlpha(pal::Ink, 0.9f * fade), barH * 0.5f);
            if (prog > 0.02f) {
                ImVec2 f1(b0.x + barW * prog, b1.y);
                dl->AddRectFilledMultiColor(ImVec2(b0.x + barH * 0.25f, b0.y + 3 * unit), ImVec2(f1.x - barH * 0.25f, f1.y - 3 * unit),
                                            pal::WithAlpha(pal::Pink, fade), pal::WithAlpha(pal::Lime, fade),
                                            pal::WithAlpha(pal::Lime, fade), pal::WithAlpha(pal::Pink, fade));
                /* 先頭の丸いインク */
                dl->AddCircleFilled(ImVec2(f1.x - barH * 0.3f, (b0.y + b1.y) * 0.5f), barH * 0.55f, pal::WithAlpha(pal::Lime, fade), 20);
            }
            dl->AddRect(b0, b1, pal::WithAlpha(pal::White, 0.8f * fade), barH * 0.5f, 0, 2.0f * unit);

            /* LOADING... + パーセント */
            char label[32];
            int dots = ((int) (t * 3.0f)) % 4;
            snprintf(label, sizeof(label), "LOADING%.*s  %d%%", dots, "...", (int) (prog * 100.0f));
            float lsz = 20.0f * unit;
            ImVec2 lts = ImGui::GetFont()->CalcTextSizeA(lsz, FLT_MAX, 0.0f, label);
            dl->AddText(ImGui::GetFont(), lsz, ImVec2(mid.x - lts.x * 0.5f, b1.y + 12 * unit),
                        pal::WithAlpha(pal::Cyan, fade), label);

            return true;
        }
    }

    /* ------------------------------------------------------------------ */
    /*  メニュー本体                                                       */
    /* ------------------------------------------------------------------ */

    /* ウィンドウ上部の派手なヘッダー */
    void DrawHeader(float time) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 62.0f;
        ImVec2 p1(p0.x + w, p0.y + h);

        dl->AddRectFilledMultiColor(p0, p1, pal::Pink, pal::Purple, pal::Cyan, pal::Pink);
        /* ヘッダーから垂れるインク */
        for (int i = 0; i < 6; i++) {
            float x = p0.x + w * (0.08f + 0.17f * i);
            float len = 8.0f + 6.0f * (float) ((i * 5) % 3) + sinf(time * 2.0f + i) * 3.0f;
            ImU32 col = (i % 2) ? pal::Purple : pal::Pink;
            dl->AddRectFilled(ImVec2(x - 5, p1.y - 2), ImVec2(x + 5, p1.y + len), col, 5.0f);
            dl->AddCircleFilled(ImVec2(x, p1.y + len), 6.0f, col, 16);
        }
        /* キラッと光る粒 */
        for (int i = 0; i < 5; i++) {
            float k = fmodf(time * 0.6f + i * 0.2f, 1.0f);
            dl->AddCircleFilled(ImVec2(p0.x + w * k, p0.y + 10 + 8 * i), 2.5f, pal::WithAlpha(pal::White, 0.7f), 8);
        }

        DrawBouncyTitle(dl, "CHROMA RUSH", ImVec2(p0.x + w * 0.5f, p0.y + h * 0.5f), 34.0f, time, 1.0f);
        ImGui::Dummy(ImVec2(w, h + 22));
    }

    /* 色付きの丸いチップ + チーム名 */
    void TeamBadge(const char* name, ImVec4 color) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetTextLineHeight() * 0.65f;
        ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f);
        DrawSplat(dl, c, r, ImGui::GetColorU32(color), 3);
        dl->AddCircle(c, r, pal::White, 24, 2.0f);
        ImGui::Dummy(ImVec2(r * 2.6f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s TEAM", name);
    }

    void DrawTeamRow(int team) {
        TeamOverride& o = g_Teams[team];
        ImGui::PushID(team);

        teamcolor::Color4f* block = teamcolor::GetTeamBlock(team);
        ImVec4 current = block ? ImVec4(block[0].r, block[0].g, block[0].b, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1);

        TeamBadge(o.name, current);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
        ImGui::TextDisabled("now");

        /* オン/オフを大きなピル型ボタンで */
        ImVec4 on  = ImVec4(0.78f, 1.00f, 0.00f, 1.0f);
        ImVec4 off = ImVec4(0.35f, 0.25f, 0.50f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        o.enabled ? on : off);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, o.enabled ? ImVec4(0.88f, 1.0f, 0.4f, 1.0f) : ImVec4(0.50f, 0.35f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          o.enabled ? ImVec4(0.08f, 0.05f, 0.15f, 1.0f) : ImVec4(1, 1, 1, 1));
        if (ImGui::Button(o.enabled ? "OVERRIDE  ON" : "OVERRIDE  OFF", ImVec2(-1, 0))) {
            o.enabled = !o.enabled;
        }
        ImGui::PopStyleColor(3);

        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3("##rgb", o.color,
                          ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB);

        ImGui::PopID();
    }

    void ApplyStyle() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 18.0f;
        style.ChildRounding     = 14.0f;
        style.FrameRounding     = 14.0f;  /* ピル型 */
        style.GrabRounding      = 14.0f;
        style.PopupRounding     = 14.0f;
        style.WindowBorderSize  = 3.0f;
        style.FrameBorderSize   = 0.0f;
        style.WindowPadding     = ImVec2(16, 14);
        style.FramePadding      = ImVec2(12, 7);
        style.ItemSpacing       = ImVec2(10, 10);
        style.GrabMinSize       = 16.0f;
        style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

        ImVec4* c = style.Colors;
        const ImVec4 pink   = ImVec4(1.00f, 0.24f, 0.65f, 1.00f);
        const ImVec4 pinkHi = ImVec4(1.00f, 0.45f, 0.78f, 1.00f);
        const ImVec4 lime   = ImVec4(0.78f, 1.00f, 0.00f, 1.00f);
        const ImVec4 cyan   = ImVec4(0.10f, 0.83f, 0.95f, 1.00f);
        const ImVec4 purple = ImVec4(0.48f, 0.18f, 1.00f, 1.00f);

        c[ImGuiCol_Text]             = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        c[ImGuiCol_TextDisabled]     = ImVec4(0.75f, 0.70f, 0.90f, 1.00f);
        c[ImGuiCol_WindowBg]         = ImVec4(0.11f, 0.06f, 0.20f, 0.95f);
        c[ImGuiCol_Border]           = pink;
        c[ImGuiCol_TitleBg]          = purple;
        c[ImGuiCol_TitleBgActive]    = pink;
        c[ImGuiCol_TitleBgCollapsed] = purple;
        c[ImGuiCol_FrameBg]          = ImVec4(0.22f, 0.13f, 0.38f, 1.00f);
        c[ImGuiCol_FrameBgHovered]   = ImVec4(0.32f, 0.18f, 0.52f, 1.00f);
        c[ImGuiCol_FrameBgActive]    = ImVec4(0.40f, 0.20f, 0.62f, 1.00f);
        c[ImGuiCol_CheckMark]        = lime;
        c[ImGuiCol_SliderGrab]       = lime;
        c[ImGuiCol_SliderGrabActive] = cyan;
        c[ImGuiCol_Button]           = pink;
        c[ImGuiCol_ButtonHovered]    = pinkHi;
        c[ImGuiCol_ButtonActive]     = cyan;
        c[ImGuiCol_Header]           = purple;
        c[ImGuiCol_HeaderHovered]    = pink;
        c[ImGuiCol_HeaderActive]     = cyan;
        c[ImGuiCol_Separator]        = ImVec4(1.00f, 0.24f, 0.65f, 0.55f);
        c[ImGuiCol_ScrollbarGrab]    = pink;
        c[ImGuiCol_NavHighlight]     = lime;
        c[ImGuiCol_PopupBg]          = ImVec4(0.14f, 0.08f, 0.26f, 0.98f);
    }

    void OnPreInit() {}

    void OnPostInit() {
        ImGui::GetIO().FontGlobalScale = 1.4f;
        ApplyStyle();
        intro::Restart();
    }

    void OnNewFrame() {
        ApplyTeamColors();
    }

    void OnRender() {
        /* ロード画面の間はメニューを出さない */
        if (intro::Draw()) return;

        float time = (float) ImGui::GetTime();

        ImGui::SetNextWindowPos(ImVec2(32, 32), ImGuiCond_FirstUseEver);
        /* 幅は固定、高さは中身に合わせて自動 */
        ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Always);

        if (ImGui::Begin("Chroma Rush", nullptr, ImGuiWindowFlags_NoTitleBar)) {
            DrawHeader(time);

            ImGui::TextColored(ImVec4(0.78f, 1.00f, 0.00f, 1.0f), "L3 + R3");
            ImGui::SameLine();
            ImGui::TextDisabled("toggle controller input");

            ImGui::Spacing();
            if (ImGui::CollapsingHeader("INK COLORS", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                DrawTeamRow(0);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                DrawTeamRow(1);
            }

            ImGui::Spacing();
            if (ImGui::Button("REPLAY INTRO", ImVec2(-1, 0))) {
                intro::Restart();
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
