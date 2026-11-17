#include "lib.hpp"

#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>

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
    };

    TeamOverride g_Teams[teamcolor::TeamCount] = {
        { "Alpha", false, { 1.00f, 0.35f, 0.00f } },
        { "Bravo", false, { 0.10f, 0.40f, 1.00f } },
    };

    /*
     * ゲームの「チームの色をセットする関数」(0x13cb40c)。
     *   void Set(Color4f* block, const Color4f* baseColor, u64 a, u64 b, float f)
     * 基本色(sRGB)から 17 種類の色バリエーションを計算して block に書き込む。
     * 表は 8 ブロック(4 チーム × 2 セット)ある。
     */
    using SetBlockFn = void (*)(teamcolor::Color4f*, const teamcolor::Color4f*, u64, u64, float);
    SetBlockFn g_OrigSetBlock = nullptr;

    constexpr int SetCount = 2;   /* 表のセット数 */
    constexpr int SetTeams = 4;   /* 1セットあたりのチーム数 */

    /* ゲームが最後にセットしたときの引数(色を作り直すために使う) */
    struct BlockState {
        bool  valid = false;
        teamcolor::Color4f original{};  /* ゲーム本来の基本色 */
        u64   a = 0, b = 0;
        float f = 0.0f;
    };
    BlockState g_Blocks[teamcolor::TeamCount][SetCount];

    teamcolor::Color4f* GetBlock(int team, int set) {
        teamcolor::Color4f* base = teamcolor::GetTeamBlock(0);
        if (base == nullptr) return nullptr;
        auto* p = reinterpret_cast<u8*>(base) + (team + set * SetTeams) * teamcolor::TeamStride;
        return reinterpret_cast<teamcolor::Color4f*>(p);
    }

    /* ブロック内の基本色(+0x110) */
    teamcolor::Color4f* GetBaseColor(int team) {
        teamcolor::Color4f* block = GetBlock(team, 0);
        return block ? reinterpret_cast<teamcolor::Color4f*>(reinterpret_cast<u8*>(block) + 0x110) : nullptr;
    }

    /* フックから呼ばれる: ゲームが色をセットするとき、上書き中なら自分の色にすり替える */
    void OnSetBlock(teamcolor::Color4f* block, const teamcolor::Color4f* color, u64 a, u64 b, float f) {
        for (int team = 0; team < teamcolor::TeamCount; team++) {
            for (int set = 0; set < SetCount; set++) {
                if (block != GetBlock(team, set)) continue;

                BlockState& s = g_Blocks[team][set];
                s.valid = true;
                s.original = *color;
                s.a = a; s.b = b; s.f = f;

                const TeamOverride& o = g_Teams[team];
                if (o.enabled) {
                    teamcolor::Color4f mine{ o.color[0], o.color[1], o.color[2], color->a };
                    g_OrigSetBlock(block, &mine, a, b, f);
                    return;
                }
                g_OrigSetBlock(block, color, a, b, f);
                return;
            }
        }
        g_OrigSetBlock(block, color, a, b, f);
    }

    /* メニューで変えた色を、ゲームと同じ計算ですぐに作り直す */
    void ReapplyTeam(int team) {
        if (g_OrigSetBlock == nullptr) return;
        const TeamOverride& o = g_Teams[team];
        for (int set = 0; set < SetCount; set++) {
            BlockState& s = g_Blocks[team][set];
            teamcolor::Color4f* block = GetBlock(team, set);
            if (!s.valid || block == nullptr) continue;
            if (o.enabled) {
                teamcolor::Color4f mine{ o.color[0], o.color[1], o.color[2], s.original.a };
                g_OrigSetBlock(block, &mine, s.a, s.b, s.f);
            } else {
                g_OrigSetBlock(block, &s.original, s.a, s.b, s.f);
            }
        }
    }

    /* 毎フレーム呼ぶ。設定が変わったチームだけ作り直す。 */
    void ApplyTeamColors() {
        static bool  lastEnabled[teamcolor::TeamCount] = {};
        static float lastColor[teamcolor::TeamCount][3] = {};
        for (int team = 0; team < teamcolor::TeamCount; team++) {
            const TeamOverride& o = g_Teams[team];
            bool changed = o.enabled != lastEnabled[team];
            if (o.enabled) {
                for (int c = 0; c < 3; c++) changed |= o.color[c] != lastColor[team][c];
            }
            if (!changed) continue;

            ReapplyTeam(team);
            lastEnabled[team] = o.enabled;
            for (int c = 0; c < 3; c++) lastColor[team][c] = o.color[c];
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
    /*  コントローラー操作                                                 */
    /* ------------------------------------------------------------------ */

}

/* imgui-xeno の入力ヘルパーが持つフラグ(L3+R3 で切り替わる)。 */
class InputHelper {
public:
    static bool toggleInput; /* メニュー操作中か */
    static bool isReadInput; /* imgui-xeno 自身がパッドを読んでいる最中か */
};

namespace chroma {

    bool IsMenuOpen() { return InputHelper::toggleInput; }
    void CloseMenu() { InputHelper::toggleInput = false; }

    bool Pressed(ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); }
    bool PressedUp()    { return Pressed(ImGuiKey_GamepadDpadUp)    || Pressed(ImGuiKey_GamepadLStickUp); }
    bool PressedDown()  { return Pressed(ImGuiKey_GamepadDpadDown)  || Pressed(ImGuiKey_GamepadLStickDown); }
    /* 値の増減は十字キーの左右だけ(左スティックの左は「戻る」に使う) */
    bool HeldLeft()     { return ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft); }
    bool HeldRight()    { return ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight); }
    bool PressedLeft()  { return Pressed(ImGuiKey_GamepadDpadLeft, false); }
    bool PressedRight() { return Pressed(ImGuiKey_GamepadDpadRight, false); }
    bool PressedDecide(){ return Pressed(ImGuiKey_GamepadL1, false); }          /* L で決定 */
    bool PressedBack()  { return Pressed(ImGuiKey_GamepadLStickLeft, false); }  /* 左スティックを左に倒すと戻る/閉じる */

    /* ------------------------------------------------------------------ */
    /*  台形パネル(上の辺が短い)                                         */
    /* ------------------------------------------------------------------ */

    struct Trap {
        ImVec2 pos;    /* 左上(下の辺の左端の x) */
        float  w, h;   /* 下の辺の幅、高さ */
        float  inset;  /* 上の辺の左右の食い込み */

        /* 高さ y での左右の端 */
        float Left(float y) const  { return pos.x + inset * (1.0f - (y - pos.y) / h); }
        float Right(float y) const { return pos.x + w - inset * (1.0f - (y - pos.y) / h); }
    };

    void FillTrap(ImDrawList* dl, ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 col) {
        ImVec2 pts[4] = { tl, tr, br, bl };
        dl->AddConvexPolyFilled(pts, 4, col);
    }

    void OutlineTrap(ImDrawList* dl, ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 col, float th) {
        ImVec2 pts[4] = { tl, tr, br, bl };
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, th);
    }

    /* 帯(パネルの y0〜y1 の範囲を切り出した台形) */
    void FillBand(ImDrawList* dl, const Trap& t, float y0, float y1, float pad, ImU32 col) {
        FillTrap(dl, ImVec2(t.Left(y0) + pad, y0), ImVec2(t.Right(y0) - pad, y0),
                 ImVec2(t.Right(y1) - pad, y1), ImVec2(t.Left(y1) + pad, y1), col);
    }

    void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* s) {
        dl->AddText(ImGui::GetFont(), size, pos, col, s);
    }

    float TextW(float size, const char* s) {
        return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }

    /* ------------------------------------------------------------------ */
    /*  メニューの状態                                                     */
    /* ------------------------------------------------------------------ */

    enum class Page { Main, Color };

    namespace menu {
        Page  g_Page      = Page::Main;
        int   g_Cursor    = 0;
        int   g_Team      = 0;
        float g_OpenAnim  = 0.0f;  /* 0..1: 開くアニメーション */
        float g_PageAnim  = 1.0f;  /* ページ切り替え時のぽよん */
        float g_HoldTime  = 0.0f;  /* 左右を押し続けている時間 */
        float g_HoldAccum = 0.0f;
    }

    const char* const MainItems[]  = { "INK COLOR", "REPLAY INTRO", "CLOSE MENU" };
    const char* const ColorItems[] = { "TEAM", "OVERRIDE", "RED", "GREEN", "BLUE", "BACK" };
    constexpr int MainCount  = 3;
    constexpr int ColorCount = 6;

    void GoTo(Page p) {
        menu::g_Page = p;
        menu::g_Cursor = 0;
        menu::g_PageAnim = 0.0f;
    }

    int ToByte(float v) { return (int) (Clamp01(v) * 255.0f + 0.5f); }

    /* 左右で RGB の値を増減(押し続けると速くなる) */
    void AdjustChannel(float& channel, float dt) {
        int dir = 0;
        if (HeldLeft()) dir = -1;
        if (HeldRight()) dir = 1;
        if (dir == 0) {
            menu::g_HoldTime = 0.0f;
            menu::g_HoldAccum = 0.0f;
            return;
        }

        int value = ToByte(channel);
        if (PressedLeft() || PressedRight()) {
            value += dir;
        } else {
            menu::g_HoldTime += dt;
            if (menu::g_HoldTime > 0.35f) {
                float speed = 20.0f + 180.0f * Clamp01((menu::g_HoldTime - 0.35f) / 1.5f); /* 1秒あたり */
                menu::g_HoldAccum += speed * dt;
                int steps = (int) menu::g_HoldAccum;
                menu::g_HoldAccum -= (float) steps;
                value += dir * steps;
            }
        }
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        channel = (float) value / 255.0f;
    }

    void HandleInput(float dt) {
        int count = menu::g_Page == Page::Main ? MainCount : ColorCount;

        if (PressedUp())   menu::g_Cursor = (menu::g_Cursor + count - 1) % count;
        if (PressedDown()) menu::g_Cursor = (menu::g_Cursor + 1) % count;

        if (menu::g_Page == Page::Main) {
            if (PressedDecide()) {
                switch (menu::g_Cursor) {
                    case 0: GoTo(Page::Color); break;
                    case 1: intro::Restart(); CloseMenu(); break;
                    case 2: CloseMenu(); break;
                }
            }
            if (PressedBack()) CloseMenu();
            return;
        }

        /* 色のページ */
        TeamOverride& o = g_Teams[menu::g_Team];
        switch (menu::g_Cursor) {
            case 0: /* TEAM */
                if (PressedDecide() || PressedLeft() || PressedRight()) menu::g_Team ^= 1;
                break;
            case 1: /* OVERRIDE */
                if (PressedDecide() || PressedLeft() || PressedRight()) o.enabled = !o.enabled;
                break;
            case 2: AdjustChannel(o.color[0], dt); break;
            case 3: AdjustChannel(o.color[1], dt); break;
            case 4: AdjustChannel(o.color[2], dt); break;
            case 5: /* BACK */
                if (PressedDecide()) GoTo(Page::Main);
                break;
        }
        if (PressedBack()) GoTo(Page::Main);
    }

    /* ------------------------------------------------------------------ */
    /*  描画                                                               */
    /* ------------------------------------------------------------------ */

    /* メニューを閉じているときの小さなタブ */
    void DrawClosedTab(ImDrawList* dl, float unit, float time) {
        Trap t{ ImVec2(24 * unit, 18 * unit), 250 * unit, 54 * unit, 22 * unit };
        FillTrap(dl, ImVec2(t.Left(t.pos.y), t.pos.y), ImVec2(t.Right(t.pos.y), t.pos.y),
                 ImVec2(t.pos.x + t.w, t.pos.y + t.h), ImVec2(t.pos.x, t.pos.y + t.h), pal::WithAlpha(pal::Ink, 0.75f));
        FillBand(dl, t, t.pos.y, t.pos.y + 6 * unit, 0, pal::Pink);
        DrawBouncyTitle(dl, "CHROMA RUSH", ImVec2(t.pos.x + t.w * 0.5f, t.pos.y + 22 * unit), 22 * unit, time, 1.0f);
        const char* hint = "L3 + R3  OPEN";
        float hs = 14 * unit;
        Text(dl, hs, ImVec2(t.pos.x + (t.w - TextW(hs, hint)) * 0.5f, t.pos.y + 36 * unit), pal::WithAlpha(pal::Lime, 0.9f), hint);
    }

    /* 1行分(選択中はライムのピル) */
    void DrawRow(ImDrawList* dl, const Trap& t, float y, float rowH, float unit, bool selected, float time,
                 const char* label, const char* value, ImU32 valueCol) {
        float pad = 18 * unit;
        float y0 = y + 4 * unit, y1 = y + rowH - 4 * unit;
        float fs = 24 * unit;
        float ty = (y0 + y1) * 0.5f - fs * 0.5f;

        if (selected) {
            float wob = sinf(time * 8.0f) * 2.0f * unit;
            FillBand(dl, t, y0 - wob * 0.5f, y1 + wob * 0.5f, pad, pal::Lime);
            /* 跳ねる矢印 */
            float ax = t.Left(y) + pad + 10 * unit + fabsf(sinf(time * 7.0f)) * 5 * unit;
            float ay = (y0 + y1) * 0.5f;
            dl->AddTriangleFilled(ImVec2(ax, ay - 8 * unit), ImVec2(ax + 12 * unit, ay), ImVec2(ax, ay + 8 * unit), pal::Ink);
        } else {
            FillBand(dl, t, y0, y1, pad, pal::WithAlpha(IM_COL32(255, 255, 255, 255), 0.08f));
        }

        ImU32 textCol = selected ? pal::Ink : pal::White;
        Text(dl, fs, ImVec2(t.Left(y) + pad + 34 * unit, ty), textCol, label);
        if (value) {
            float vw = TextW(fs, value);
            Text(dl, fs, ImVec2(t.Right(y) - pad - 18 * unit - vw, ty), selected ? pal::Ink : valueCol, value);
        }
    }

    /* RGB の行: 数値 + その色のバー */
    void DrawChannelRow(ImDrawList* dl, const Trap& t, float y, float rowH, float unit, bool selected, float time,
                        const char* label, float v, ImU32 chanCol) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%3d", ToByte(v));
        DrawRow(dl, t, y, rowH, unit, selected, time, label, buf, chanCol);

        float pad = 18 * unit;
        float bx0 = t.Left(y) + pad + 140 * unit;
        float bx1 = t.Right(y) - pad - 80 * unit;
        float by = y + rowH * 0.5f;
        dl->AddRectFilled(ImVec2(bx0, by - 5 * unit), ImVec2(bx1, by + 5 * unit), pal::WithAlpha(pal::Ink, 0.6f), 5 * unit);
        dl->AddRectFilled(ImVec2(bx0, by - 5 * unit), ImVec2(bx0 + (bx1 - bx0) * Clamp01(v), by + 5 * unit), chanCol, 5 * unit);
        dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * Clamp01(v), by), 8 * unit, pal::White, 16);
    }

    void DrawMenu(ImDrawList* dl, float unit, float time) {
        int count = menu::g_Page == Page::Main ? MainCount : ColorCount;
        float rowH = 50 * unit;
        float headerH = 78 * unit;
        float previewH = menu::g_Page == Page::Color ? 96 * unit : 0.0f;
        float footerH = 40 * unit;
        float h = headerH + rowH * count + previewH + footerH + 16 * unit;

        /* 開くとき: 上からぽよんと落ちてくる */
        float open = EaseOutBack(menu::g_OpenAnim);
        float drop = (1.0f - open) * -120 * unit;
        Trap t{ ImVec2(36 * unit, 30 * unit + drop), 540 * unit, h, 70 * unit };

        ImVec2 tl(t.Left(t.pos.y), t.pos.y), tr(t.Right(t.pos.y), t.pos.y);
        ImVec2 br(t.pos.x + t.w, t.pos.y + t.h), bl(t.pos.x, t.pos.y + t.h);

        /* 影 → 本体 → 枠 */
        float sh = 8 * unit;
        FillTrap(dl, ImVec2(tl.x + sh, tl.y + sh), ImVec2(tr.x + sh, tr.y + sh), ImVec2(br.x + sh, br.y + sh),
                 ImVec2(bl.x + sh, bl.y + sh), pal::WithAlpha(IM_COL32(0, 0, 0, 255), 0.35f));
        FillTrap(dl, tl, tr, br, bl, pal::WithAlpha(IM_COL32(30, 14, 58, 255), 0.94f));

        /* ヘッダー帯(ピンク)と、そこから垂れるインク */
        float hy1 = t.pos.y + headerH - 14 * unit;
        FillBand(dl, t, t.pos.y, hy1, 0, pal::Pink);
        for (int i = 0; i < 7; i++) {
            float x = t.Left(hy1) + (t.Right(hy1) - t.Left(hy1)) * (0.08f + 0.14f * i);
            float len = (6.0f + 7.0f * (float) ((i * 5) % 3) + sinf(time * 2.0f + i) * 3.0f) * unit;
            dl->AddRectFilled(ImVec2(x - 5 * unit, hy1 - 2 * unit), ImVec2(x + 5 * unit, hy1 + len), pal::Pink, 5 * unit);
            dl->AddCircleFilled(ImVec2(x, hy1 + len), 6 * unit, pal::Pink, 16);
        }
        const char* title = menu::g_Page == Page::Main ? "CHROMA RUSH" : "INK COLOR";
        float pop = EaseOutBack(menu::g_PageAnim);
        DrawBouncyTitle(dl, title, ImVec2(t.pos.x + t.w * 0.5f, t.pos.y + 32 * unit), 36 * unit * (0.6f + 0.4f * pop), time, 1.0f);

        /* 行 */
        float y = t.pos.y + headerH;
        if (menu::g_Page == Page::Main) {
            for (int i = 0; i < MainCount; i++, y += rowH) {
                DrawRow(dl, t, y, rowH, unit, menu::g_Cursor == i, time, MainItems[i], i == 0 ? ">" : nullptr, pal::Cyan);
            }
        } else {
            TeamOverride& o = g_Teams[menu::g_Team];
            char teamBuf[24];
            snprintf(teamBuf, sizeof(teamBuf), "< %s >", menu::g_Team == 0 ? "ALPHA" : "BRAVO");
            DrawRow(dl, t, y, rowH, unit, menu::g_Cursor == 0, time, ColorItems[0], teamBuf, pal::Cyan);  y += rowH;
            DrawRow(dl, t, y, rowH, unit, menu::g_Cursor == 1, time, ColorItems[1], o.enabled ? "ON" : "OFF",
                    o.enabled ? pal::Lime : pal::WithAlpha(pal::White, 0.5f));                            y += rowH;
            DrawChannelRow(dl, t, y, rowH, unit, menu::g_Cursor == 2, time, ColorItems[2], o.color[0], IM_COL32(255, 70, 90, 255));  y += rowH;
            DrawChannelRow(dl, t, y, rowH, unit, menu::g_Cursor == 3, time, ColorItems[3], o.color[1], IM_COL32(90, 230, 90, 255));  y += rowH;
            DrawChannelRow(dl, t, y, rowH, unit, menu::g_Cursor == 4, time, ColorItems[4], o.color[2], IM_COL32(80, 140, 255, 255)); y += rowH;
            DrawRow(dl, t, y, rowH, unit, menu::g_Cursor == 5, time, ColorItems[5], nullptr, pal::White);  y += rowH;

            /* プレビュー: いまの色 → 新しい色 */
            teamcolor::Color4f* base = GetBaseColor(menu::g_Team);
            ImU32 nowCol = base ? ImGui::GetColorU32(ImVec4(base->r, base->g, base->b, 1.0f)) : pal::WithAlpha(pal::White, 0.3f);
            ImU32 newCol = ImGui::GetColorU32(ImVec4(o.color[0], o.color[1], o.color[2], 1.0f));
            float cy = y + previewH * 0.5f;
            float cx = t.pos.x + t.w * 0.5f;
            float r = 26 * unit;
            DrawSplat(dl, ImVec2(cx - 110 * unit, cy), r, nowCol, 2);
            DrawSplat(dl, ImVec2(cx + 110 * unit, cy), r * (1.0f + 0.06f * sinf(time * 5.0f)), newCol, 5);
            dl->AddTriangleFilled(ImVec2(cx - 14 * unit, cy - 12 * unit), ImVec2(cx + 14 * unit, cy), ImVec2(cx - 14 * unit, cy + 12 * unit), pal::Lime);
            float ls = 16 * unit;
            Text(dl, ls, ImVec2(cx - 110 * unit - TextW(ls, "NOW") * 0.5f, cy + r + 10 * unit), pal::WithAlpha(pal::White, 0.8f), "NOW");
            Text(dl, ls, ImVec2(cx + 110 * unit - TextW(ls, "NEW") * 0.5f, cy + r + 10 * unit), pal::WithAlpha(pal::White, 0.8f), "NEW");
            y += previewH;
        }

        /* 操作ガイド */
        const char* guide = "D-PAD MOVE   L SELECT   L-STICK LEFT BACK";
        float gs = 16 * unit;
        float gy = t.pos.y + t.h - footerH * 0.5f - gs * 0.5f - 4 * unit;
        Text(dl, gs, ImVec2(t.pos.x + (t.w - TextW(gs, guide)) * 0.5f, gy), pal::WithAlpha(pal::Cyan, 0.9f), guide);

        OutlineTrap(dl, tl, tr, br, bl, pal::Pink, 4 * unit);
    }

    void OnPreInit() {}

    void OnPostInit() {
        ImGui::GetIO().FontGlobalScale = 1.4f;
        intro::Restart();
    }

    void OnNewFrame() {
        ApplyTeamColors();
    }

    void OnRender() {
        /* ロード画面の間はメニューを出さない */
        if (intro::Draw()) return;

        ImGuiIO& io = ImGui::GetIO();
        float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : (1.0f / 60.0f);
        float time = (float) ImGui::GetTime();
        float unit = io.DisplaySize.y / 720.0f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        if (!IsMenuOpen()) {
            menu::g_OpenAnim = 0.0f;
            DrawClosedTab(dl, unit, time);
            return;
        }

        if (menu::g_OpenAnim == 0.0f) GoTo(Page::Main);
        menu::g_OpenAnim = Clamp01(menu::g_OpenAnim + dt / 0.35f);
        menu::g_PageAnim = Clamp01(menu::g_PageAnim + dt / 0.3f);

        HandleInput(dt);
        if (IsMenuOpen()) DrawMenu(dl, unit, time);
    }

}

/* ------------------------------------------------------------------ */
/*  メニュー操作中はゲームにボタン入力を渡さない                       */
/* ------------------------------------------------------------------ */

namespace nn::hid {
    struct NpadFullKeyState;
    struct NpadHandheldState;
    struct NpadJoyDualState;
    struct NpadJoyLeftState;
    struct NpadJoyRightState;

    int GetNpadStates(NpadFullKeyState*, int, const unsigned int&);
    int GetNpadStates(NpadHandheldState*, int, const unsigned int&);
    int GetNpadStates(NpadJoyDualState*, int, const unsigned int&);
    int GetNpadStates(NpadJoyLeftState*, int, const unsigned int&);
    int GetNpadStates(NpadJoyRightState*, int, const unsigned int&);
}

namespace chroma {
    /* NpadBaseState: +0x08 ボタン, +0x10 左スティック, +0x18 右スティック(1つ 0x28 バイト) */
    void BlockPadStates(void* states, int count) {
        if (!InputHelper::toggleInput || InputHelper::isReadInput || states == nullptr) return;
        for (int i = 0; i < count; i++) {
            u8* s = reinterpret_cast<u8*>(states) + i * 0x28;
            std::memset(s + 0x08, 0, 0x18);
        }
    }
}

#define DEFINE_PAD_HOOK(Name, StateType)                                              \
    HOOK_DEFINE_TRAMPOLINE(Name) {                                                    \
        static int Callback(nn::hid::StateType* states, int count, const unsigned int& port) { \
            int r = Orig(states, count, port);                                        \
            chroma::BlockPadStates(states, count);                                    \
            return r;                                                                 \
        }                                                                             \
    }

DEFINE_PAD_HOOK(PadFullKeyHook,  NpadFullKeyState);
DEFINE_PAD_HOOK(PadHandheldHook, NpadHandheldState);
DEFINE_PAD_HOOK(PadJoyDualHook,  NpadJoyDualState);
DEFINE_PAD_HOOK(PadJoyLeftHook,  NpadJoyLeftState);
DEFINE_PAD_HOOK(PadJoyRightHook, NpadJoyRightState);

/* チームの色をセットする関数 (main 0x13cb40c) */
HOOK_DEFINE_TRAMPOLINE(TeamColorSetHook) {
    static void CallOrig(chroma::teamcolor::Color4f* block, const chroma::teamcolor::Color4f* color, u64 a, u64 b, float f) {
        Orig(block, color, a, b, f);
    }

    static void Callback(chroma::teamcolor::Color4f* block, const chroma::teamcolor::Color4f* color, u64 a, u64 b, float f) {
        chroma::OnSetBlock(block, color, a, b, f);
    }
};

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

    chroma::g_OrigSetBlock = &TeamColorSetHook::CallOrig;
    TeamColorSetHook::InstallAtOffset(0x13cb40c);

    using FullKeyFn  = int (*)(nn::hid::NpadFullKeyState*, int, const unsigned int&);
    using HandheldFn = int (*)(nn::hid::NpadHandheldState*, int, const unsigned int&);
    using JoyDualFn  = int (*)(nn::hid::NpadJoyDualState*, int, const unsigned int&);
    using JoyLeftFn  = int (*)(nn::hid::NpadJoyLeftState*, int, const unsigned int&);
    using JoyRightFn = int (*)(nn::hid::NpadJoyRightState*, int, const unsigned int&);
    PadFullKeyHook::InstallAtFuncPtr(static_cast<FullKeyFn>(nn::hid::GetNpadStates));
    PadHandheldHook::InstallAtFuncPtr(static_cast<HandheldFn>(nn::hid::GetNpadStates));
    PadJoyDualHook::InstallAtFuncPtr(static_cast<JoyDualFn>(nn::hid::GetNpadStates));
    PadJoyLeftHook::InstallAtFuncPtr(static_cast<JoyLeftFn>(nn::hid::GetNpadStates));
    PadJoyRightHook::InstallAtFuncPtr(static_cast<JoyRightFn>(nn::hid::GetNpadStates));
}

extern "C" NORETURN void exl_exception_entry() {
    /* Note: this is only applicable in the context of applets/sysmodules. */
    EXL_ABORT("Default exception handler called!");
}
