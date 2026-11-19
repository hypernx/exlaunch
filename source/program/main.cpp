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
 * 操作: L でメニューを開く。十字キー上下で選択、左右で値を変える、L で決定、左スティックを左で戻る/閉じる。
 *
 * メニューは作り直し中: いまは「SETTINGS(フォント・メニューの大きさ・文字の大きさ)」だけ。
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

    namespace bw {
        constexpr ImU32 Black = IM_COL32(0, 0, 0, 255);
        constexpr ImU32 White = IM_COL32(255, 255, 255, 255);
        constexpr ImU32 Gray  = IM_COL32(150, 150, 150, 255);
        constexpr ImU32 Panel = IM_COL32(8, 8, 8, 230);
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
        constexpr float BakeSize = 36.0f;  /* 大きめに作って縮めて使う(拡大してもぼやけにくい) */

        enum Id { JetBrains = 0, SwitchStd, Proggy, Count };
        const char* const Names[Count] = { "JETBRAINS MONO", "SWITCH", "PROGGY (PIXEL)" };

        ImFont* g_Fonts[Count] = {};

        /* Switch 本体の共有フォント(読み込みが終わるまで少し待つ) */
        ImFont* AddShared(nn::pl::SharedFontType type) {
            for (int i = 0; i < 200 && nn::pl::GetSharedFontLoadState(type) != nn::pl::SharedFontLoadState_Loaded; i++) {
                nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(10));
            }
            if (nn::pl::GetSharedFontLoadState(type) != nn::pl::SharedFontLoadState_Loaded) return nullptr;
            const void* data = nn::pl::GetSharedFontAddress(type);
            size_t size = nn::pl::GetSharedFontSize(type);
            if (data == nullptr || size == 0) return nullptr;
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;  /* 本体のメモリなので ImGui に解放させない */
            cfg.OversampleH = 2;
            return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), (int) size, BakeSize, &cfg);
        }

        /* imgui-xeno が既定フォント(JetBrains Mono)を足す前に呼ばれる */
        void OnPreInit() {
            ImGuiIO& io = ImGui::GetIO();
            g_Fonts[SwitchStd]    = AddShared(nn::pl::SharedFontType_Standard);
            ImFontConfig proggy;
            proggy.SizePixels = 26.0f;
            g_Fonts[Proggy] = io.Fonts->AddFontDefault(&proggy);
        }

        /* 既定フォントは最後に足されるので、初期化のあとに拾う */
        void OnPostInit() {
            ImGuiIO& io = ImGui::GetIO();
            if (io.Fonts->Fonts.Size > 0) g_Fonts[JetBrains] = io.Fonts->Fonts[io.Fonts->Fonts.Size - 1];
        }

        bool Available(int id) { return id >= 0 && id < Count && g_Fonts[id] != nullptr; }
    }

    /* ------------------------------------------------------------------ */
    /*  設定(SD に保存)                                                  */
    /* ------------------------------------------------------------------ */

    namespace settings {
        constexpr const char* Path = "sd:/ChromaRush/settings.txt";
        constexpr int ScaleMin = 50, ScaleMax = 200, ScaleStep = 5;

        int  g_Font      = fonts::JetBrains;
        int  g_MenuScale = 100;  /* % */
        int  g_TextScale = 100;  /* % */
        bool g_Loaded    = false;
        bool g_Dirty     = false;

        int ClampScale(int v) { return v < ScaleMin ? ScaleMin : (v > ScaleMax ? ScaleMax : v); }

        void Load() {
            g_Loaded = true;
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
            g_Font = fonts::JetBrains;
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
            dl->AddRectFilled(p0, p1, pal::WithAlpha(IM_COL32(8, 8, 8, 255), 0.92f * fade));
            dl->AddRect(p0, p1, pal::WithAlpha(pal::White, fade), 0, 0, 2 * unit);
            if (pop < 0.9f) return true;

            {
                const char* logo = "CHROMA RUSH";
                float ts = 30 * unit;
                float tw = TextW(ts, logo);
                Text(dl, ts, ImVec2(c.x - tw * 0.5f, c.y - 22 * unit - ts * 0.5f), pal::WithAlpha(pal::White, fade), logo);
            }

            float bw_ = 240 * unit, bh = 10 * unit;
            ImVec2 b0(c.x - bw_ * 0.5f, c.y + 12 * unit), b1(c.x + bw_ * 0.5f, b0.y + bh);
            float prog = Clamp01(t / (Duration - 0.2f));
            prog = 1.0f - (1.0f - prog) * (1.0f - prog);
            dl->AddRect(b0, b1, pal::WithAlpha(pal::White, fade), 0, 0, 1.5f * unit);
            if (prog > 0.01f) {
                dl->AddRectFilled(ImVec2(b0.x + 3 * unit, b0.y + 3 * unit), ImVec2(b0.x + 3 * unit + (bw_ - 6 * unit) * prog, b1.y - 3 * unit),
                                  pal::WithAlpha(pal::White, fade));
            }

            char label[24];
            snprintf(label, sizeof(label), "LOADING %d%%", (int) (prog * 100.0f));
            float ls = 14 * unit;
            float lw = TextW(ls, label);
            Text(dl, ls, ImVec2(c.x - lw * 0.5f, b1.y + 8 * unit), pal::WithAlpha(IM_COL32(170, 170, 170, 255), fade), label);

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
    }

    const char* const MainItems[]     = { "SETTINGS" };
    const char* const SettingsItems[] = { "FONT", "MENU SIZE", "TEXT SIZE", "RESET" };
    constexpr int MainCount     = sizeof(MainItems) / sizeof(MainItems[0]);
    constexpr int SettingsCount = sizeof(SettingsItems) / sizeof(SettingsItems[0]);

    void GoTo(Page p) {
        if (menu::g_Page == Page::Settings && p != Page::Settings && settings::g_Dirty) settings::Save();
        menu::g_Page = p;
        menu::g_Cursor = 0;
    }

    void Close() {
        if (settings::g_Dirty) settings::Save();
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

    void HandleInput() {
        int count = menu::g_Page == Page::Main ? MainCount : SettingsCount;
        if (menu::g_Cursor >= count) menu::g_Cursor = count - 1;
        bool canDecide = menu::g_OpenTime > 0.25f;  /* 開いたときの L を決定と間違えない */

        if (PressedUp())   menu::g_Cursor = (menu::g_Cursor + count - 1) % count;
        if (PressedDown()) menu::g_Cursor = (menu::g_Cursor + 1) % count;

        if (menu::g_Page == Page::Main) {
            if (canDecide && PressedDecide() && menu::g_Cursor == 0) GoTo(Page::Settings);
            if (PressedBack()) Close();
            return;
        }

        int dir = PressedRightR() ? 1 : (PressedLeftR() ? -1 : 0);
        switch (menu::g_Cursor) {
            case 0: /* FONT */
                if (dir) StepFont(dir);
                else if (canDecide && PressedDecide()) StepFont(1);
                break;
            case 1: /* MENU SIZE */
                if (dir) StepScale(settings::g_MenuScale, dir);
                break;
            case 2: /* TEXT SIZE */
                if (dir) StepScale(settings::g_TextScale, dir);
                break;
            case 3: /* RESET */
                if (canDecide && PressedDecide()) settings::Reset();
                break;
        }
        if (PressedBack()) GoTo(Page::Main);
    }

    /* ------------------------------------------------------------------ */
    /*  描画(白と黒のシンプルなスタイル)                                 */
    /* ------------------------------------------------------------------ */

    struct Layout {
        float x, w, unit, rowH, pad, fs;
    };

    /* 1行分。選択中は白地に黒文字 */
    void DrawRow(ImDrawList* dl, const Layout& L, float y, bool selected, const char* label, const char* value) {
        float y0 = y + 3 * L.unit, y1 = y + L.rowH - 3 * L.unit;
        if (selected) dl->AddRectFilled(ImVec2(L.x + L.pad * 0.5f, y0), ImVec2(L.x + L.w - L.pad * 0.5f, y1), bw::White);
        ImU32 col = selected ? bw::Black : bw::White;
        float ty = (y0 + y1) * 0.5f - L.fs * 0.5f;
        Text(dl, L.fs, ImVec2(L.x + L.pad, ty), col, label);
        if (value) Text(dl, L.fs, ImVec2(L.x + L.w - L.pad - TextW(L.fs, value), ty), selected ? bw::Black : bw::Gray, value);
    }

    /* メニューを閉じているときの小さな表示 */
    void DrawClosedTab(ImDrawList* dl, float unit, float text) {
        const char* s = "CHROMA RUSH   L : MENU";
        float fs = 14 * unit * text;
        ImVec2 p0(20 * unit, 16 * unit);
        ImVec2 p1(p0.x + TextW(fs, s) + 20 * unit, p0.y + fs + 12 * unit);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 160));
        Text(dl, fs, ImVec2(p0.x + 10 * unit, p0.y + 6 * unit), IM_COL32(255, 255, 255, 200), s);
    }

    void DrawMenu(ImDrawList* dl, float unit, float text) {
        bool settingsPage = menu::g_Page == Page::Settings;
        int count = settingsPage ? SettingsCount : MainCount;
        float fs = 20 * unit * text;
        /* 文字を大きくしたときに行からはみ出さないよう、行の高さと幅も文字に合わせて広げる */
        float rowH = (fs + 24 * unit) > 44 * unit ? (fs + 24 * unit) : 44 * unit;
        Layout L{ 36 * unit, 460 * unit * (text > 1.0f ? text : 1.0f), unit, rowH, 18 * unit, fs };

        float ts = 26 * unit * text;
        float headerH = ts + 28 * unit;
        float gs = 13 * unit * text;
        float footerH = gs + 21 * unit;
        float h = headerH + L.rowH * count + footerH;
        float top = 30 * unit;

        float alpha = Clamp01(menu::g_OpenTime / 0.15f);
        dl->AddRectFilled(ImVec2(L.x, top), ImVec2(L.x + L.w, top + h), pal::WithAlpha(bw::Panel, 0.9f * alpha));
        dl->AddRect(ImVec2(L.x, top), ImVec2(L.x + L.w, top + h), bw::White, 0, 0, 2 * unit);

        const char* title = settingsPage ? "SETTINGS" : "CHROMA RUSH";
        Text(dl, ts, ImVec2(L.x + L.pad, top + (headerH - ts) * 0.5f), bw::White, title);
        dl->AddLine(ImVec2(L.x + L.pad * 0.5f, top + headerH - 1 * unit), ImVec2(L.x + L.w - L.pad * 0.5f, top + headerH - 1 * unit), bw::White, 1.5f * unit);

        float y = top + headerH;
        char buf[48];
        if (!settingsPage) {
            for (int i = 0; i < MainCount; i++, y += L.rowH) DrawRow(dl, L, y, menu::g_Cursor == i, MainItems[i], ">");
        } else {
            snprintf(buf, sizeof(buf), "< %s >", fonts::Names[settings::g_Font]);
            DrawRow(dl, L, y, menu::g_Cursor == 0, SettingsItems[0], buf); y += L.rowH;
            snprintf(buf, sizeof(buf), "< %d%% >", settings::g_MenuScale);
            DrawRow(dl, L, y, menu::g_Cursor == 1, SettingsItems[1], buf); y += L.rowH;
            snprintf(buf, sizeof(buf), "< %d%% >", settings::g_TextScale);
            DrawRow(dl, L, y, menu::g_Cursor == 2, SettingsItems[2], buf); y += L.rowH;
            DrawRow(dl, L, y, menu::g_Cursor == 3, SettingsItems[3], nullptr); y += L.rowH;
        }

        const char* guide = settingsPage ? "D-PAD LEFT/RIGHT CHANGE   L-STICK LEFT BACK" : "L SELECT   L-STICK LEFT CLOSE";
        Text(dl, gs, ImVec2(L.x + L.pad, top + h - footerH + 8 * unit), bw::Gray, guide);
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
            DrawClosedTab(dl, unit, text);
            return;
        }

        if (!menu::g_WasOpen) {
            GoTo(Page::Main);
            menu::g_OpenTime = 0.0f;
            menu::g_WasOpen = true;
        }
        menu::g_OpenTime += dt;

        HandleInput();
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
