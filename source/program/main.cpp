#include "lib.hpp"
#include "nn/os.hpp"

#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <cstring>

#include <imgui.h>
#include <imgui_xeno.h>

#include "recorder.hpp"
#include "storage.hpp"

/*
 * Chroma Rush
 *
 * NVN の nvnBootstrapLoader をフックして imgui-xeno に渡し、
 * ゲームの画面表示の直前に ImGui のメニューを重ね描きする。
 * 操作: L でメニューを開く。十字キー上下で選択、L で決定(設定の項目は右に調整パネルが出て、左右で値を変える)、
 *       左スティックを左で戻る/閉じる。
 *
 * メニューは作り直し中: いまは「設定(フォント・メニューの大きさ・文字の大きさ)」だけ。
 * 見た目: 白基調・黒文字。フォントは Switch 本体のフォント + フリーフォント 10 種類
 * (tools/fonts.txt。ビルド時にメニューで使う文字だけを取り出して埋め込む)。
 * 設定は sd:/ChromaRush/settings.txt に保存される。
 */

namespace nn::pl {
    enum SharedFontType {
        SharedFontType_Standard = 0,
        SharedFontType_ChineseSimple = 1,
        SharedFontType_ChineseSimpleExtension = 2,
        SharedFontType_ChineseTraditional = 3,
        SharedFontType_Korean = 4,
        SharedFontType_NintendoExtension = 5,
    };
    enum SharedFontLoadState {
        SharedFontLoadState_Loading = 0,
        SharedFontLoadState_Loaded = 1,
    };
    SharedFontLoadState GetSharedFontLoadState(SharedFontType type);
    const void* GetSharedFontAddress(SharedFontType type);
    size_t GetSharedFontSize(SharedFontType type);
}

namespace chroma {

    float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    /* ------------------------------------------------------------------ */
    /*  見た目                                                             */
    /* ------------------------------------------------------------------ */

    namespace pal {
        constexpr ImU32 White = IM_COL32(255, 255, 255, 255);

        ImU32 WithAlpha(ImU32 c, float a) {
            int ai = (int) (a * 255.0f);
            if (ai < 0) ai = 0;
            if (ai > 255) ai = 255;
            return (c & ~IM_COL32_A_MASK) | ((ImU32) ai << IM_COL32_A_SHIFT);
        }
    }

    /* 白基調・黒文字 */
    namespace theme {
        constexpr ImU32 Panel    = IM_COL32(255, 255, 255, 245);  /* メニューの地 */
        constexpr ImU32 Shadow   = IM_COL32(0, 0, 0, 60);         /* うっすら影 */
        constexpr ImU32 Border   = IM_COL32(210, 210, 210, 255);  /* 枠・区切り線 */
        constexpr ImU32 Text     = IM_COL32(20, 20, 20, 255);     /* 文字 */
        constexpr ImU32 SubText  = IM_COL32(110, 110, 110, 255);  /* 値・説明 */
        constexpr ImU32 Select   = IM_COL32(235, 235, 235, 255);  /* 選択中の行 */
        constexpr ImU32 Accent   = IM_COL32(20, 20, 20, 255);     /* 選択中の行の左の線 */
    }

    /* イージング: ぽよんと跳ねる */
    float EaseOutBack(float t) {
        t = Clamp01(t);
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }

    /* ------------------------------------------------------------------ */
    /*  フォント                                                           */
    /* ------------------------------------------------------------------ */

    namespace fonts {
        struct EmbeddedFont { const char* name; const unsigned char* data; size_t size; };
#include "generated/fonts_data.inc"

        constexpr int EmbeddedCount = sizeof(kEmbeddedFonts) / sizeof(kEmbeddedFonts[0]);
        constexpr int Count = 1 + EmbeddedCount;  /* 0 = Switch 本体のフォント */
        constexpr float BakeSize = 28.0f;         /* 少し大きめに作って縮めて使う */

        ImFont* g_Fonts[Count] = {};

        const char* Name(int id) { return id == 0 ? "Switch 本体" : kEmbeddedFonts[id - 1].name; }

        ImFontConfig MakeConfig() {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;  /* 元のデータは ImGui に解放させない */
            cfg.OversampleH = 1;
            cfg.OversampleV = 1;
            cfg.PixelSnapH = true;
            return cfg;
        }

        /* Switch 本体の共有フォント(読み込みが終わるまで少し待つ) */
        ImFont* AddShared(nn::pl::SharedFontType type) {
            for (int i = 0; i < 200 && nn::pl::GetSharedFontLoadState(type) != nn::pl::SharedFontLoadState_Loaded; i++) {
                nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(10));
            }
            if (nn::pl::GetSharedFontLoadState(type) != nn::pl::SharedFontLoadState_Loaded) return nullptr;
            const void* data = nn::pl::GetSharedFontAddress(type);
            size_t size = nn::pl::GetSharedFontSize(type);
            if (data == nullptr || size == 0) return nullptr;
            ImFontConfig cfg = MakeConfig();
            return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), (int) size, BakeSize, &cfg, kGlyphRanges);
        }

        /* imgui-xeno が既定フォントを足す前に呼ばれる。メニューで使う文字だけを焼き込む */
        void OnPreInit() {
            ImGuiIO& io = ImGui::GetIO();
            g_Fonts[0] = AddShared(nn::pl::SharedFontType_Standard);
            for (int i = 0; i < EmbeddedCount; i++) {
                ImFontConfig cfg = MakeConfig();
                g_Fonts[1 + i] = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kEmbeddedFonts[i].data),
                                                                (int) kEmbeddedFonts[i].size, BakeSize, &cfg, kGlyphRanges);
            }
        }

        void OnPostInit() {}

        bool Available(int id) { return id >= 0 && id < Count && g_Fonts[id] != nullptr; }

        /* 最初に使えるフォント(Switch 本体 → 埋め込みの順) */
        int First() {
            for (int i = 0; i < Count; i++) if (Available(i)) return i;
            return 0;
        }
    }

    /* ------------------------------------------------------------------ */
    /*  設定(SD に保存)                                                  */
    /* ------------------------------------------------------------------ */

    namespace settings {
        constexpr const char* Path = "sd:/ChromaRush/settings.txt";
        constexpr int ScaleMin = 50, ScaleMax = 200, ScaleStep = 5;

        int  g_Font      = 0;
        int  g_MenuScale = 100;  /* % */
        int  g_TextScale = 100;  /* % */
        bool g_Loaded    = false;
        bool g_Dirty     = false;

        int ClampScale(int v) { return v < ScaleMin ? ScaleMin : (v > ScaleMax ? ScaleMax : v); }

        void Load() {
            g_Loaded = true;
            g_Font = fonts::First();
            if (!storage::EnsureSdMounted()) return;
            nn::fs::FileHandle h;
            if (R_FAILED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Read))) return;
            char buf[256] = {};
            long size = 0;
            nn::fs::GetFileSize(&size, h);
            if (size > 0 && size < (long) sizeof(buf)) nn::fs::ReadFile(h, 0, buf, (ulong) size);
            nn::fs::CloseFile(h);

            for (char* line = buf; line && *line; ) {
                char* next = std::strchr(line, '\n');
                if (next) *next++ = '\0';
                char* eq = std::strchr(line, '=');
                if (eq) {
                    *eq = '\0';
                    int v = std::atoi(eq + 1);
                    if (!std::strcmp(line, "font") && fonts::Available(v)) g_Font = v;
                    else if (!std::strcmp(line, "menu")) g_MenuScale = ClampScale(v);
                    else if (!std::strcmp(line, "text")) g_TextScale = ClampScale(v);
                }
                line = next;
            }
        }

        void Save() {
            g_Dirty = false;
            if (!storage::EnsureSdMounted()) return;
            char buf[96];
            int n = std::snprintf(buf, sizeof(buf), "font=%d\nmenu=%d\ntext=%d\n", g_Font, g_MenuScale, g_TextScale);
            nn::fs::DeleteFile(Path);
            if (R_FAILED(nn::fs::CreateFile(Path, n))) return;
            nn::fs::FileHandle h;
            if (R_FAILED(nn::fs::OpenFile(&h, Path, nn::fs::OpenMode_Write))) return;
            nn::fs::WriteFile(h, 0, buf, (u64) n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }

        void Reset() {
            g_Font = fonts::First();
            g_MenuScale = 100;
            g_TextScale = 100;
            g_Dirty = true;
        }
    }

    ImFont* Font() {
        if (fonts::Available(settings::g_Font)) return fonts::g_Fonts[settings::g_Font];
        return ImGui::GetFont();
    }

    void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* s) {
        dl->AddText(Font(), size, pos, col, s);
    }

    float TextW(float size, const char* s) {
        return Font()->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }

    /* ------------------------------------------------------------------ */
    /*  ロード画面                                                         */
    /* ------------------------------------------------------------------ */

    namespace intro {
        constexpr float Duration = 2.4f;   /* ロードの長さ(秒) */
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

            float fade = t > Duration ? 1.0f - (t - Duration) / FadeOut : Clamp01(t / 0.25f);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 ds = io.DisplaySize;
            float unit = ds.y / 720.0f;

            float cw = 320 * unit, ch = 120 * unit;
            float pop = EaseOutBack(t / 0.4f);
            ImVec2 c(ds.x * 0.5f, ds.y * 0.5f);
            ImVec2 p0(c.x - cw * 0.5f * pop, c.y - ch * 0.5f * pop), p1(c.x + cw * 0.5f * pop, c.y + ch * 0.5f * pop);
            dl->AddRectFilled(ImVec2(p0.x + 4 * unit, p0.y + 4 * unit), ImVec2(p1.x + 4 * unit, p1.y + 4 * unit), pal::WithAlpha(theme::Shadow, fade));
            dl->AddRectFilled(p0, p1, pal::WithAlpha(theme::Panel, fade));
            dl->AddRect(p0, p1, pal::WithAlpha(theme::Border, fade), 0, 0, 1.5f * unit);
            if (pop < 0.9f) return true;

            {
                const char* logo = "CHROMA RUSH";
                float ts = 30 * unit;
                float tw = TextW(ts, logo);
                Text(dl, ts, ImVec2(c.x - tw * 0.5f, c.y - 22 * unit - ts * 0.5f), pal::WithAlpha(theme::Text, fade), logo);
            }

            float bw_ = 240 * unit, bh = 10 * unit;
            ImVec2 b0(c.x - bw_ * 0.5f, c.y + 12 * unit), b1(c.x + bw_ * 0.5f, b0.y + bh);
            float prog = Clamp01(t / (Duration - 0.2f));
            prog = 1.0f - (1.0f - prog) * (1.0f - prog);
            dl->AddRect(b0, b1, pal::WithAlpha(theme::Text, fade), 0, 0, 1.5f * unit);
            if (prog > 0.01f) {
                dl->AddRectFilled(ImVec2(b0.x + 3 * unit, b0.y + 3 * unit), ImVec2(b0.x + 3 * unit + (bw_ - 6 * unit) * prog, b1.y - 3 * unit),
                                  pal::WithAlpha(theme::Text, fade));
            }

            char label[40];
            snprintf(label, sizeof(label), "読み込み中 %d%%", (int) (prog * 100.0f));
            float ls = 14 * unit;
            float lw = TextW(ls, label);
            Text(dl, ls, ImVec2(c.x - lw * 0.5f, b1.y + 8 * unit), pal::WithAlpha(theme::SubText, fade), label);

            return true;
        }
    }
}

/* imgui-xeno の入力ヘルパーが持つフラグ */
class InputHelper {
public:
    static bool toggleInput; /* メニュー操作中か */
    static bool isReadInput; /* imgui-xeno 自身がパッドを読んでいる最中か */
};

namespace chroma {

    bool IsMenuOpen() { return InputHelper::toggleInput; }
    void OpenMenu()  { InputHelper::toggleInput = true; }
    void CloseMenu() { InputHelper::toggleInput = false; }

    bool Pressed(ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); }
    bool PressedUp()    { return Pressed(ImGuiKey_GamepadDpadUp)   || Pressed(ImGuiKey_GamepadLStickUp); }
    bool PressedDown()  { return Pressed(ImGuiKey_GamepadDpadDown) || Pressed(ImGuiKey_GamepadLStickDown); }
    bool PressedLeftR() { return Pressed(ImGuiKey_GamepadDpadLeft); }   /* 押し続けると連続 */
    bool PressedRightR(){ return Pressed(ImGuiKey_GamepadDpadRight); }
    bool PressedDecide(){ return Pressed(ImGuiKey_GamepadL1, false); }          /* L で決定 */
    bool PressedBack()  { return Pressed(ImGuiKey_GamepadLStickLeft, false); }  /* 左スティックを左: 戻る/閉じる */

    /* ------------------------------------------------------------------ */
    /*  メニュー                                                           */
    /* ------------------------------------------------------------------ */

    enum class Page { Main, Settings };

    namespace menu {
        Page  g_Page     = Page::Main;
        int   g_Cursor   = 0;
        float g_OpenTime = 0.0f;
        bool  g_WasOpen  = false;
        bool  g_Editing  = false;  /* 右の調整パネルを開いているか */
        float g_PopTime  = 0.0f;   /* 調整パネルが出てからの時間(ぽんと出る演出用) */
        float g_NoteTime = -1.0f;  /* 「元に戻しました」を出している時間 */
    }

    const char* const MainItems[]     = { "設定" };
    const char* const SettingsItems[] = { "フォント", "メニューの大きさ", "文字の大きさ", "元に戻す" };
    constexpr int MainCount     = sizeof(MainItems) / sizeof(MainItems[0]);
    constexpr int SettingsCount = sizeof(SettingsItems) / sizeof(SettingsItems[0]);

    void StopEditing() {
        menu::g_Editing = false;
        if (settings::g_Dirty) settings::Save();
    }

    void GoTo(Page p) {
        StopEditing();
        menu::g_Page = p;
        menu::g_Cursor = 0;
        menu::g_NoteTime = -1.0f;
    }

    void Close() {
        StopEditing();
        CloseMenu();
    }

    /* 使えるフォントだけを順番に切り替える */
    void StepFont(int dir) {
        int f = settings::g_Font;
        for (int i = 0; i < fonts::Count; i++) {
            f = (f + dir + fonts::Count) % fonts::Count;
            if (fonts::Available(f)) break;
        }
        if (f != settings::g_Font) { settings::g_Font = f; settings::g_Dirty = true; }
    }

    void StepScale(int& v, int dir) {
        int n = settings::ClampScale(v + dir * settings::ScaleStep);
        if (n != v) { v = n; settings::g_Dirty = true; }
    }

    void HandleInput(float dt) {
        bool canDecide = menu::g_OpenTime > 0.25f;  /* 開いたときの L を決定と間違えない */
        if (menu::g_NoteTime >= 0.0f) { menu::g_NoteTime += dt; if (menu::g_NoteTime > 1.2f) menu::g_NoteTime = -1.0f; }

        /* 調整パネルを開いている間: 左右で値を変える。L か 左スティック左 で閉じる */
        if (menu::g_Editing) {
            menu::g_PopTime += dt;
            int dir = PressedRightR() ? 1 : (PressedLeftR() ? -1 : 0);
            switch (menu::g_Cursor) {
                case 0: if (dir) StepFont(dir); break;
                case 1: if (dir) StepScale(settings::g_MenuScale, dir); break;
                case 2: if (dir) StepScale(settings::g_TextScale, dir); break;
            }
            if ((canDecide && PressedDecide()) || PressedBack()) StopEditing();
            return;
        }

        int count = menu::g_Page == Page::Main ? MainCount : SettingsCount;
        if (menu::g_Cursor >= count) menu::g_Cursor = count - 1;
        if (PressedUp())   menu::g_Cursor = (menu::g_Cursor + count - 1) % count;
        if (PressedDown()) menu::g_Cursor = (menu::g_Cursor + 1) % count;

        if (menu::g_Page == Page::Main) {
            if (canDecide && PressedDecide() && menu::g_Cursor == 0) GoTo(Page::Settings);
            if (PressedBack()) Close();
            return;
        }

        if (canDecide && PressedDecide()) {
            if (menu::g_Cursor <= 2) {          /* 調整パネルを出す */
                menu::g_Editing = true;
                menu::g_PopTime = 0.0f;
                menu::g_NoteTime = -1.0f;
            } else {                            /* 元に戻す */
                settings::Reset();
                settings::Save();
                menu::g_NoteTime = 0.0f;
            }
        }
        if (PressedBack()) GoTo(Page::Main);
    }

    /* ------------------------------------------------------------------ */
    /*  描画(白と黒のシンプルなスタイル)                                 */
    /* ------------------------------------------------------------------ */

    struct Layout {
        float x, w, unit, rowH, pad, fs;
    };

    /* 1行分。選択中は薄いグレー + 左に黒い線 */
    void DrawRow(ImDrawList* dl, const Layout& L, float y, bool selected, const char* label, const char* mark) {
        float y0 = y + 3 * L.unit, y1 = y + L.rowH - 3 * L.unit;
        if (selected) {
            dl->AddRectFilled(ImVec2(L.x + L.pad * 0.5f, y0), ImVec2(L.x + L.w - L.pad * 0.5f, y1), theme::Select, 6 * L.unit);
            dl->AddRectFilled(ImVec2(L.x + L.pad * 0.5f, y0 + 6 * L.unit), ImVec2(L.x + L.pad * 0.5f + 4 * L.unit, y1 - 6 * L.unit), theme::Accent, 2 * L.unit);
        }
        float ty = (y0 + y1) * 0.5f - L.fs * 0.5f;
        Text(dl, L.fs, ImVec2(L.x + L.pad + 4 * L.unit, ty), theme::Text, label);
        if (mark) Text(dl, L.fs, ImVec2(L.x + L.w - L.pad - TextW(L.fs, mark), ty), selected ? theme::Text : theme::SubText, mark);
    }

    /* 白いパネル(うっすら影つき) */
    void DrawPanel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float unit, float alpha) {
        float r = 10 * unit;
        dl->AddRectFilled(ImVec2(p0.x + 4 * unit, p0.y + 5 * unit), ImVec2(p1.x + 4 * unit, p1.y + 5 * unit), pal::WithAlpha(theme::Shadow, alpha), r);
        dl->AddRectFilled(p0, p1, pal::WithAlpha(theme::Panel, alpha), r);
        dl->AddRect(p0, p1, pal::WithAlpha(theme::Border, alpha), r, 0, 1.0f * unit);
    }

    /* 選んだ項目の右に、ぽんと出る小さなパネル(値 + 操作の説明) */
    void DrawPopout(ImDrawList* dl, float x, float rowY, float rowH, float unit, float text,
                    const char* value, float valueW, const char* hint, float t) {
        float fs = 20 * unit * text, hs = 12 * unit * text;
        float pad = 14 * unit;
        float w = fmaxf(valueW, TextW(hs, hint)) + pad * 2;
        float h = fs + hs + 26 * unit;
        float cy = rowY + rowH * 0.5f;

        float s = EaseOutBack(Clamp01(t / 0.18f));      /* 左端から、ぽよんと広がる */
        float a = Clamp01(t / 0.08f);
        ImVec2 p0(x, cy - h * 0.5f * s), p1(x + w * s, cy + h * 0.5f * s);
        DrawPanel(dl, p0, p1, unit, a);
        if (s < 0.85f) return;

        float vx = x + (w - TextW(fs, value)) * 0.5f;
        Text(dl, fs, ImVec2(vx, cy - h * 0.5f + 9 * unit), theme::Text, value);
        float hx = x + (w - TextW(hs, hint)) * 0.5f;
        Text(dl, hs, ImVec2(hx, cy + h * 0.5f - hs - 8 * unit), theme::SubText, hint);
    }

    void DrawMenu(ImDrawList* dl, float unit, float text) {
        bool settingsPage = menu::g_Page == Page::Settings;
        int count = settingsPage ? SettingsCount : MainCount;
        float fs = 20 * unit * text;
        /* 文字を大きくしたときに行からはみ出さないよう、行の高さも文字に合わせて広げる */
        float rowH = (fs + 24 * unit) > 44 * unit ? (fs + 24 * unit) : 44 * unit;
        float ts = 24 * unit * text;
        float headerH = ts + 30 * unit;

        /* 幅は項目名とタイトルが入るぶんだけ(値は右のパネルに出すので、ここには入れない) */
        const char* const* items = settingsPage ? SettingsItems : MainItems;
        const char* title = settingsPage ? "設定" : "CHROMA RUSH";
        float pad = 14 * unit;
        float markW = settingsPage ? 0.0f : TextW(fs, ">") + 16 * unit;
        float labelW = 0.0f;
        for (int i = 0; i < count; i++) labelW = fmaxf(labelW, TextW(fs, items[i]));
        float w = fmaxf(pad * 2 + 4 * unit + labelW + markW, pad * 2 + 4 * unit + TextW(ts, title));
        Layout L{ 24 * unit, w, unit, rowH, pad, fs };
        float h = headerH + L.rowH * count + 8 * unit;
        float top = 24 * unit;

        float alpha = Clamp01(menu::g_OpenTime / 0.15f);
        DrawPanel(dl, ImVec2(L.x, top), ImVec2(L.x + L.w, top + h), unit, alpha);
        Text(dl, ts, ImVec2(L.x + L.pad + 4 * unit, top + (headerH - ts) * 0.5f), theme::Text, title);
        dl->AddLine(ImVec2(L.x + L.pad * 0.5f, top + headerH - 1 * unit), ImVec2(L.x + L.w - L.pad * 0.5f, top + headerH - 1 * unit), theme::Border, 1.0f * unit);

        float y = top + headerH;
        for (int i = 0; i < count; i++, y += L.rowH) {
            DrawRow(dl, L, y, menu::g_Cursor == i, items[i], settingsPage ? nullptr : ">");
        }

        if (!settingsPage) return;
        float rowY = top + headerH + L.rowH * menu::g_Cursor;
        float px = L.x + L.w + 8 * unit;

        /* 元に戻したとき: 少しだけお知らせを出す */
        if (menu::g_NoteTime >= 0.0f && menu::g_Cursor == 3) {
            const char* note = "元に戻しました";
            DrawPopout(dl, px, rowY, L.rowH, unit, text, note, TextW(fs, note), "", menu::g_NoteTime);
            return;
        }
        if (!menu::g_Editing) return;

        /* 調整パネル: 幅は候補のうちいちばん長いもので固定(切り替えても幅が揺れない) */
        char buf[64], tmp[64];
        float valueW = 0.0f;
        if (menu::g_Cursor == 0) {
            for (int i = 0; i < fonts::Count; i++) {
                if (!fonts::Available(i)) continue;
                snprintf(tmp, sizeof(tmp), "< %s >", fonts::Name(i));
                valueW = fmaxf(valueW, TextW(fs, tmp));
            }
            snprintf(buf, sizeof(buf), "< %s >", fonts::Name(settings::g_Font));
        } else {
            valueW = TextW(fs, "< 200% >");
            snprintf(buf, sizeof(buf), "< %d%% >", menu::g_Cursor == 1 ? settings::g_MenuScale : settings::g_TextScale);
        }
        DrawPopout(dl, px, rowY, L.rowH, unit, text, buf, valueW, "十字キー左右：変える　L：決定", menu::g_PopTime);
    }

    void OnPreInit() {
        fonts::OnPreInit();
    }

    void OnPostInit() {
        fonts::OnPostInit();
        intro::Restart();
    }

    void OnNewFrame() {}

    void OnRender() {
        /* 設定は最初の描画のときに読む(SD の準備ができてから) */
        if (!settings::g_Loaded && !intro::g_Active) settings::Load();

        /* ロード画面の間はメニューを出さない */
        if (intro::Draw()) return;

        ImGuiIO& io = ImGui::GetIO();
        float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : (1.0f / 60.0f);
        float unit = io.DisplaySize.y / 720.0f * (settings::g_MenuScale / 100.0f);
        float text = settings::g_TextScale / 100.0f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        if (!IsMenuOpen()) {
            if (menu::g_WasOpen && settings::g_Dirty) settings::Save();
            menu::g_WasOpen = false;
            return;  /* 閉じている間は何も表示しない */
        }

        if (!menu::g_WasOpen) {
            GoTo(Page::Main);
            menu::g_OpenTime = 0.0f;
            menu::g_Editing = false;
            menu::g_WasOpen = true;
        }
        menu::g_OpenTime += dt;

        HandleInput(dt);
        if (IsMenuOpen()) DrawMenu(dl, unit, text);
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
    /*
     * NpadBaseState: +0x08 ボタン, +0x10 左スティック, +0x18 右スティック(1つ 0x28 バイト)
     * ボタンのビット: L = 6
     */
    constexpr u64 ButtonL = 1ull << 6;
    bool g_PrevL[5] = {};

    void BlockPadStates(int kind, void* states, int count) {
        if (states == nullptr || count <= 0 || InputHelper::isReadInput) return;
        u8* first = reinterpret_cast<u8*>(states);

        /* メニューが閉じているとき: L を押した瞬間にメニューを開く(その L はゲームに渡さない) */
        if (!InputHelper::toggleInput) {
            u64 buttons;
            std::memcpy(&buttons, first + 0x08, sizeof(buttons));
            bool l = (buttons & ButtonL) != 0;
            bool pressed = l && !g_PrevL[kind];
            g_PrevL[kind] = l;
            if (!pressed || intro::g_Active) return;
            OpenMenu();
        } else {
            g_PrevL[kind] = true;  /* 閉じた直後に、押しっぱなしの L で開き直さない */
        }

        /* メニュー操作中: ボタンとスティックをゲームに渡さない */
        for (int i = 0; i < count; i++) {
            std::memset(first + i * 0x28 + 0x08, 0, 0x18);
        }
    }
}

#define DEFINE_PAD_HOOK(Name, StateType, Kind)                                        \
    HOOK_DEFINE_TRAMPOLINE(Name) {                                                    \
        static int Callback(nn::hid::StateType* states, int count, const unsigned int& port) { \
            int r = Orig(states, count, port);                                        \
            chroma::BlockPadStates(Kind, states, count);                              \
            return r;                                                                 \
        }                                                                             \
    }

DEFINE_PAD_HOOK(PadFullKeyHook,  NpadFullKeyState,  0);
DEFINE_PAD_HOOK(PadHandheldHook, NpadHandheldState, 1);
DEFINE_PAD_HOOK(PadJoyDualHook,  NpadJoyDualState,  2);
DEFINE_PAD_HOOK(PadJoyLeftHook,  NpadJoyLeftState,  3);
DEFINE_PAD_HOOK(PadJoyRightHook, NpadJoyRightState, 4);

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

    /* 実機調査用の記録(ヒーローモード / サイドオーダー / 機内モード中だけ記録) */
    chroma::recorder::Install();

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
