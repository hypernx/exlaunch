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

    float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

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

    /*
     * 「いまゲームの中にコピーされている色」の控え。
     * キャラや地面は、試合の開始時に表から色を写して使い続けるので、
     * 表を書き換えただけでは変わらない。そこで、この控えと同じ値を
     * ゲームのメモリから探して、新しい色に置き換える(メモリ同期)。
     */
    constexpr int BlockColors = 18;   /* 17 バリエーション + 基本色(+0x110) */

    struct Committed {
        bool valid = false;
        teamcolor::Color4f colors[BlockColors]{};
    };
    Committed g_Committed[teamcolor::TeamCount][SetCount];

    void SnapshotBlock(int team, int set) {
        teamcolor::Color4f* block = GetBlock(team, set);
        if (block == nullptr) return;
        for (int k = 0; k < BlockColors; k++) g_Committed[team][set].colors[k] = block[k];
        g_Committed[team][set].valid = true;
    }

    /* ------------------------------------------------------------------ */
    /*  メモリ同期(少しずつ、毎フレーム数MBずつ探して置き換える)         */
    /* ------------------------------------------------------------------ */

    namespace sync {
        struct Pair { u32 o[4]; u32 n[4]; };
        constexpr int MaxPairs = teamcolor::TeamCount * SetCount * BlockColors;
        constexpr size_t BytesPerFrame = 12u << 20;

        Pair      g_Pairs[MaxPairs];
        int       g_PairCount = 0;
        u64       g_Bloom[65536 / 64];
        bool      g_Active = false;
        uintptr_t g_Next = 0, g_Cur = 0, g_End = 0;
        u64       g_Total = 0, g_Done = 0;
        int       g_Replaced = 0, g_LastReplaced = -1;

        u32 Bits(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }
        u32 Hash(u32 w) { return (w ^ (w >> 16)) & 0xFFFF; }

        bool IsTrivial(const teamcolor::Color4f& c) {
            auto t = [](float v) { return v == 0.0f || v == 1.0f; };
            return t(c.r) && t(c.g) && t(c.b);
        }

        bool Scannable(const MemoryInfo& mi) {
            if ((mi.perm & Perm_Rw) != Perm_Rw) return false;
            switch (mi.type & MemState_Type) {
                case MemType_Normal: case MemType_CodeMutable: case MemType_Heap:
                case MemType_ModuleCodeMutable: case MemType_MappedMemory:
                case MemType_TransferMem: case MemType_TransferMemIsolated:
                    break;
                default:
                    return false;
            }
            /* 自分自身(このプラグイン)は除く */
            const auto& self = exl::util::GetSelfModuleInfo().m_Total;
            if (mi.addr < self.GetEnd() && mi.addr + mi.size > self.m_Start) return false;
            return true;
        }

        /* 次に調べるメモリ領域へ進む。無ければ false */
        bool NextRegion() {
            while (true) {
                MemoryInfo mi{}; u32 pi;
                if (R_FAILED(svcQueryMemory(&mi, &pi, g_Next))) return false;
                uintptr_t end = mi.addr + mi.size;
                if (mi.size == 0 || end <= g_Next) return false;  /* 末尾 */
                g_Next = end;
                if (Scannable(mi)) { g_Cur = mi.addr; g_End = end; return true; }
            }
        }

        u64 CountTotal() {
            u64 total = 0;
            uintptr_t next = 0;
            while (true) {
                MemoryInfo mi{}; u32 pi;
                if (R_FAILED(svcQueryMemory(&mi, &pi, next))) break;
                uintptr_t end = mi.addr + mi.size;
                if (mi.size == 0 || end <= next) break;
                next = end;
                if (Scannable(mi)) total += mi.size;
            }
            return total;
        }

        void Start() {
            std::memset(g_Bloom, 0, sizeof(g_Bloom));
            for (int i = 0; i < g_PairCount; i++) {
                u32 h = Hash(g_Pairs[i].o[0]);
                g_Bloom[h >> 6] |= 1ull << (h & 63);
            }
            g_Next = 0; g_Cur = g_End = 0;
            g_Total = CountTotal(); g_Done = 0; g_Replaced = 0;
            g_Active = g_PairCount > 0;
        }

        /* RGB の3つが一致したら RGB だけ書き換える(アルファはそのまま) */
        void ScanRange(uintptr_t start, size_t bytes) {
            volatile u32* p = reinterpret_cast<volatile u32*>(start);
            size_t n = bytes / 4;
            if (n < 3) return;
            for (size_t i = 0; i + 2 < n; i++) {
                u32 w = p[i];
                u32 h = Hash(w);
                if (!(g_Bloom[h >> 6] & (1ull << (h & 63)))) continue;
                for (int k = 0; k < g_PairCount; k++) {
                    const Pair& pr = g_Pairs[k];
                    if (w != pr.o[0] || p[i + 1] != pr.o[1] || p[i + 2] != pr.o[2]) continue;
                    p[i] = pr.n[0]; p[i + 1] = pr.n[1]; p[i + 2] = pr.n[2];
                    g_Replaced++;
                    i += 2;
                    break;
                }
            }
        }

        void Step() {
            if (!g_Active) return;
            size_t budget = BytesPerFrame;
            while (budget > 0) {
                if (g_Cur >= g_End && !NextRegion()) {
                    g_Active = false;
                    g_LastReplaced = g_Replaced;
                    return;
                }
                /* 直前に領域が消えていないか確認してから読む */
                MemoryInfo mi{}; u32 pi;
                if (R_FAILED(svcQueryMemory(&mi, &pi, g_Cur)) || !Scannable(mi)) { g_Cur = g_End; continue; }
                uintptr_t limit = mi.addr + mi.size < g_End ? mi.addr + mi.size : g_End;
                size_t len = limit - g_Cur;
                if (len > budget) len = budget;
                ScanRange(g_Cur, len);
                g_Cur += len; g_Done += len; budget -= len;
            }
        }

        float Progress() { return g_Total ? Clamp01((float) g_Done / (float) g_Total) : 1.0f; }
    }

    /* 控え(古い色) → 表の今の色 の対応を作って、メモリ同期を始める */
    void StartSync() {
        sync::g_PairCount = 0;
        for (int team = 0; team < teamcolor::TeamCount; team++) {
            for (int set = 0; set < SetCount; set++) {
                Committed& c = g_Committed[team][set];
                teamcolor::Color4f* block = GetBlock(team, set);
                if (!c.valid || block == nullptr) continue;
                for (int k = 0; k < BlockColors; k++) {
                    const teamcolor::Color4f& o = c.colors[k];
                    const teamcolor::Color4f& n = block[k];
                    if (o.r == n.r && o.g == n.g && o.b == n.b) continue;
                    if (sync::IsTrivial(o)) continue;  /* (1,0,0) などは他のデータと紛らわしいので触らない */
                    sync::Pair& pr = sync::g_Pairs[sync::g_PairCount++];
                    pr.o[0] = sync::Bits(o.r); pr.o[1] = sync::Bits(o.g); pr.o[2] = sync::Bits(o.b); pr.o[3] = sync::Bits(o.a);
                    pr.n[0] = sync::Bits(n.r); pr.n[1] = sync::Bits(n.g); pr.n[2] = sync::Bits(n.b); pr.n[3] = sync::Bits(n.a);
                }
                SnapshotBlock(team, set);
            }
        }
        sync::Start();
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
                } else {
                    g_OrigSetBlock(block, color, a, b, f);
                }
                /* 試合の準備でセットされた色 = これからゲームが写す色 */
                SnapshotBlock(team, set);
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
            if (!g_Committed[team][set].valid) SnapshotBlock(team, set);
            if (o.enabled) {
                teamcolor::Color4f mine{ o.color[0], o.color[1], o.color[2], s.original.a };
                g_OrigSetBlock(block, &mine, s.a, s.b, s.f);
            } else {
                g_OrigSetBlock(block, &s.original, s.a, s.b, s.f);
            }
        }
    }

    /* 毎フレーム呼ぶ。表はすぐ作り直し、手が止まったらメモリ同期 */
    void ApplyTeamColors(float dt) {
        static bool  lastEnabled[teamcolor::TeamCount] = {};
        static float lastColor[teamcolor::TeamCount][3] = {};
        static float idle = 0.0f;
        static bool  pending = false;

        bool anyChange = false;
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
            anyChange = true;
        }

        if (anyChange) { pending = true; idle = 0.0f; }
        else idle += dt;

        /* 0.3秒操作が止まって、前の同期が終わっていたら同期する */
        if (pending && idle > 0.3f && !sync::g_Active) {
            pending = false;
            StartSync();
        }
        sync::Step();
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

            /* ふわっと出て、ふわっと消える */
            float fade = t > Duration ? 1.0f - (t - Duration) / FadeOut : Clamp01(t / 0.25f);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 ds = io.DisplaySize;
            float unit = ds.y / 720.0f;

            /* 画面の真ん中に小さなカード */
            float cw = 320 * unit, ch = 120 * unit;
            float pop = EaseOutBack(t / 0.4f);
            ImVec2 c(ds.x * 0.5f, ds.y * 0.5f);
            ImVec2 p0(c.x - cw * 0.5f * pop, c.y - ch * 0.5f * pop), p1(c.x + cw * 0.5f * pop, c.y + ch * 0.5f * pop);
            dl->AddRectFilled(p0, p1, pal::WithAlpha(IM_COL32(8, 8, 8, 255), 0.92f * fade));
            dl->AddRect(p0, p1, pal::WithAlpha(pal::White, fade), 0, 0, 2 * unit);
            if (pop < 0.9f) return true;

            /* ロゴ */
            {
                const char* logo = "CHROMA RUSH";
                float ts = 30 * unit;
                float tw = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, logo).x;
                dl->AddText(ImGui::GetFont(), ts, ImVec2(c.x - tw * 0.5f, c.y - 22 * unit - ts * 0.5f), pal::WithAlpha(pal::White, fade), logo);
            }

            /* ローディングバー */
            float bw = 240 * unit, bh = 10 * unit;
            ImVec2 b0(c.x - bw * 0.5f, c.y + 12 * unit), b1(c.x + bw * 0.5f, b0.y + bh);
            float prog = Clamp01(t / (Duration - 0.2f));
            prog = 1.0f - (1.0f - prog) * (1.0f - prog);
            dl->AddRect(b0, b1, pal::WithAlpha(pal::White, fade), 0, 0, 1.5f * unit);
            if (prog > 0.01f) {
                dl->AddRectFilled(ImVec2(b0.x + 3 * unit, b0.y + 3 * unit), ImVec2(b0.x + 3 * unit + (bw - 6 * unit) * prog, b1.y - 3 * unit),
                                  pal::WithAlpha(pal::White, fade));
            }

            char label[24];
            snprintf(label, sizeof(label), "LOADING %d%%", (int) (prog * 100.0f));
            float ls = 14 * unit;
            float lw = ImGui::GetFont()->CalcTextSizeA(ls, FLT_MAX, 0.0f, label).x;
            dl->AddText(ImGui::GetFont(), ls, ImVec2(c.x - lw * 0.5f, b1.y + 8 * unit), pal::WithAlpha(IM_COL32(170, 170, 170, 255), fade), label);

            return true;
        }
    }

    /* ------------------------------------------------------------------ */
    /*  コントローラー操作                                                 */
    /* ------------------------------------------------------------------ */

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
    bool HeldLeft()     { return ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft); }
    bool HeldRight()    { return ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight); }
    bool PressedLeft()  { return Pressed(ImGuiKey_GamepadDpadLeft, false); }
    bool PressedRight() { return Pressed(ImGuiKey_GamepadDpadRight, false); }
    bool PressedDecide(){ return Pressed(ImGuiKey_GamepadL1, false); }          /* L で決定 */
    bool PressedBack()  { return Pressed(ImGuiKey_GamepadLStickLeft, false); }  /* 左スティックを左: 戻る/閉じる */

    /* ------------------------------------------------------------------ */
    /*  色の選び方: 虹色(色相) + 明るさ                                    */
    /* ------------------------------------------------------------------ */

    struct Picker {
        bool  init  = false;
        float hue   = 0.0f;  /* 0..1 虹色の位置 */
        float shade = 0.0f;  /* -1 (暗い) .. 0 (あざやか) .. 1 (白っぽい) */
    };
    Picker g_Pickers[teamcolor::TeamCount];

    void PickerToRgb(const Picker& p, float out[3]) {
        float sat = p.shade > 0.0f ? 1.0f - p.shade : 1.0f;
        float val = p.shade < 0.0f ? 1.0f + p.shade : 1.0f;
        ImGui::ColorConvertHSVtoRGB(p.hue, sat, val, out[0], out[1], out[2]);
        /* ぴったり 0 や 1 は他のデータと紛らわしいので、ほんの少しだけずらす */
        for (int i = 0; i < 3; i++) out[i] = out[i] * 0.996f + 0.002f;
    }

    void PickerFromRgb(Picker& p, const float rgb[3]) {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], h, s, v);
        p.hue = h;
        p.shade = v < 0.99f ? v - 1.0f : 1.0f - s;
        p.init = true;
    }

    Picker& GetPicker(int team) {
        Picker& p = g_Pickers[team];
        if (!p.init) PickerFromRgb(p, g_Teams[team].color);
        return p;
    }

    /* ------------------------------------------------------------------ */
    /*  メニューの状態                                                     */
    /* ------------------------------------------------------------------ */

    enum class Page { Main, Color };

    namespace menu {
        Page  g_Page     = Page::Main;
        int   g_Cursor   = 0;
        int   g_Team     = 0;
        float g_OpenTime = 0.0f;
        bool  g_WasOpen  = false;
        float g_HoldTime = 0.0f;
    }

    const char* const MainItems[]  = { "INK COLOR", "REPLAY INTRO" };
    const char* const ColorItems[] = { "TEAM", "OVERRIDE", "COLOR", "SHADE" };
    constexpr int MainCount  = 2;
    constexpr int ColorCount = 4;

    void GoTo(Page p) {
        menu::g_Page = p;
        menu::g_Cursor = 0;
    }

    /* 左右で値を動かす(押し続けると速くなる)。true なら変化した */
    bool Slide(float& v, float dt, float lo, float hi, bool wrap) {
        int dir = 0;
        if (HeldLeft()) dir = -1;
        if (HeldRight()) dir = 1;
        if (dir == 0) { menu::g_HoldTime = 0.0f; return false; }

        float step;
        if (PressedLeft() || PressedRight()) {
            step = 1.0f / 120.0f;
        } else {
            menu::g_HoldTime += dt;
            if (menu::g_HoldTime < 0.3f) return false;
            float speed = 0.15f + 0.45f * Clamp01((menu::g_HoldTime - 0.3f) / 1.2f); /* 1秒あたり */
            step = speed * dt;
        }
        v += dir * step * (hi - lo);
        if (wrap) {
            while (v < lo) v += hi - lo;
            while (v >= hi) v -= hi - lo;
        } else {
            v = v < lo ? lo : (v > hi ? hi : v);
        }
        return true;
    }

    void HandleInput(float dt) {
        int count = menu::g_Page == Page::Main ? MainCount : ColorCount;
        bool canDecide = menu::g_OpenTime > 0.25f;  /* 開いたときの L を決定と間違えない */

        if (PressedUp())   menu::g_Cursor = (menu::g_Cursor + count - 1) % count;
        if (PressedDown()) menu::g_Cursor = (menu::g_Cursor + 1) % count;

        if (menu::g_Page == Page::Main) {
            if (canDecide && PressedDecide()) {
                switch (menu::g_Cursor) {
                    case 0: GoTo(Page::Color); break;
                    case 1: intro::Restart(); CloseMenu(); break;
                }
            }
            if (PressedBack()) CloseMenu();
            return;
        }

        TeamOverride& o = g_Teams[menu::g_Team];
        Picker& p = GetPicker(menu::g_Team);
        switch (menu::g_Cursor) {
            case 0: /* TEAM */
                if ((canDecide && PressedDecide()) || PressedLeft() || PressedRight()) menu::g_Team ^= 1;
                break;
            case 1: /* OVERRIDE */
                if ((canDecide && PressedDecide()) || PressedLeft() || PressedRight()) o.enabled = !o.enabled;
                break;
            case 2: /* COLOR (虹色) */
                if (Slide(p.hue, dt, 0.0f, 1.0f, true)) PickerToRgb(p, o.color);
                break;
            case 3: /* SHADE */
                if (Slide(p.shade, dt, -1.0f, 1.0f, false)) PickerToRgb(p, o.color);
                break;
        }
        if (PressedBack()) GoTo(Page::Main);
    }

    /* ------------------------------------------------------------------ */
    /*  描画(白と黒のシンプルなスタイル)                                 */
    /* ------------------------------------------------------------------ */

    namespace bw {
        constexpr ImU32 Black = IM_COL32(0, 0, 0, 255);
        constexpr ImU32 White = IM_COL32(255, 255, 255, 255);
        constexpr ImU32 Gray  = IM_COL32(150, 150, 150, 255);
        constexpr ImU32 Panel = IM_COL32(8, 8, 8, 230);
    }

    void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* s) {
        dl->AddText(ImGui::GetFont(), size, pos, col, s);
    }

    float TextW(float size, const char* s) {
        return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }

    ImU32 HueColor(float h) {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h, 1.0f, 1.0f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
    }

    /* 虹色のバー */
    void DrawRainbow(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
        const int seg = 6;
        float w = (p1.x - p0.x) / seg;
        for (int i = 0; i < seg; i++) {
            ImU32 a = HueColor((float) i / seg), b = HueColor((float) (i + 1) / seg);
            dl->AddRectFilledMultiColor(ImVec2(p0.x + w * i, p0.y), ImVec2(p0.x + w * (i + 1) + 0.5f, p1.y), a, b, b, a);
        }
    }

    /* 暗い → あざやか → 白っぽい のバー */
    void DrawShadeBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float hue) {
        float mx = (p0.x + p1.x) * 0.5f;
        ImU32 c = HueColor(hue);
        dl->AddRectFilledMultiColor(p0, ImVec2(mx, p1.y), bw::Black, c, c, bw::Black);
        dl->AddRectFilledMultiColor(ImVec2(mx, p0.y), p1, c, bw::White, bw::White, c);
    }

    /* バーの上の目印 */
    void DrawMarker(ImDrawList* dl, float x, float y0, float y1, float unit) {
        dl->AddRectFilled(ImVec2(x - 3 * unit, y0 - 4 * unit), ImVec2(x + 3 * unit, y1 + 4 * unit), bw::White);
        dl->AddRect(ImVec2(x - 3 * unit, y0 - 4 * unit), ImVec2(x + 3 * unit, y1 + 4 * unit), bw::Black, 0, 0, 1.5f * unit);
    }

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

    /* バー付きの行(数字は出さない) */
    template<typename BarFn>
    void DrawBarRow(ImDrawList* dl, const Layout& L, float y, bool selected, const char* label, float t, BarFn drawBar) {
        DrawRow(dl, L, y, selected, label, nullptr);
        float bx0 = L.x + L.pad + 120 * L.unit, bx1 = L.x + L.w - L.pad - 6 * L.unit;
        float cy = y + L.rowH * 0.5f;
        ImVec2 b0(bx0, cy - 7 * L.unit), b1(bx1, cy + 7 * L.unit);
        drawBar(b0, b1);
        dl->AddRect(b0, b1, selected ? bw::Black : bw::Gray, 0, 0, 1.0f * L.unit);
        DrawMarker(dl, bx0 + (bx1 - bx0) * Clamp01(t), b0.y, b1.y, L.unit);
    }

    /* メニューを閉じているときの小さな表示 */
    void DrawClosedTab(ImDrawList* dl, float unit) {
        const char* s = "CHROMA RUSH   L : MENU";
        float fs = 14 * unit;
        ImVec2 p0(20 * unit, 16 * unit);
        ImVec2 p1(p0.x + TextW(fs, s) + 20 * unit, p0.y + fs + 12 * unit);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 160));
        Text(dl, fs, ImVec2(p0.x + 10 * unit, p0.y + 6 * unit), IM_COL32(255, 255, 255, 200), s);
    }

    void DrawMenu(ImDrawList* dl, float unit) {
        bool colorPage = menu::g_Page == Page::Color;
        int count = colorPage ? ColorCount : MainCount;
        Layout L{ 36 * unit, 460 * unit, unit, 44 * unit, 18 * unit, 20 * unit };

        float headerH = 54 * unit;
        float previewH = colorPage ? 70 * unit : 0.0f;
        float footerH = (sync::g_Active ? 54 : 34) * unit;
        float h = headerH + L.rowH * count + previewH + footerH;
        float top = 30 * unit;

        float alpha = Clamp01(menu::g_OpenTime / 0.15f);
        ImU32 panel = pal::WithAlpha(bw::Panel, 0.9f * alpha);

        /* 本体(長方形) + 白い枠 */
        dl->AddRectFilled(ImVec2(L.x, top), ImVec2(L.x + L.w, top + h), panel);
        dl->AddRect(ImVec2(L.x, top), ImVec2(L.x + L.w, top + h), bw::White, 0, 0, 2 * unit);

        /* ヘッダー: タイトル + 区切り線 */
        const char* title = colorPage ? "INK COLOR" : "CHROMA RUSH";
        float ts = 26 * unit;
        Text(dl, ts, ImVec2(L.x + L.pad, top + (headerH - ts) * 0.5f), bw::White, title);
        if (colorPage) {
            const char* sub = "< L-STICK";
            float ss = 14 * unit;
            Text(dl, ss, ImVec2(L.x + L.w - L.pad - TextW(ss, sub), top + (headerH - ss) * 0.5f), bw::Gray, sub);
        }
        dl->AddLine(ImVec2(L.x + L.pad * 0.5f, top + headerH - 1 * unit), ImVec2(L.x + L.w - L.pad * 0.5f, top + headerH - 1 * unit), bw::White, 1.5f * unit);

        float y = top + headerH;
        if (!colorPage) {
            for (int i = 0; i < MainCount; i++, y += L.rowH) {
                DrawRow(dl, L, y, menu::g_Cursor == i, MainItems[i], i == 0 ? ">" : nullptr);
            }
        } else {
            TeamOverride& o = g_Teams[menu::g_Team];
            Picker& p = GetPicker(menu::g_Team);

            DrawRow(dl, L, y, menu::g_Cursor == 0, ColorItems[0], menu::g_Team == 0 ? "< ALPHA >" : "< BRAVO >"); y += L.rowH;
            DrawRow(dl, L, y, menu::g_Cursor == 1, ColorItems[1], o.enabled ? "ON" : "OFF");                    y += L.rowH;
            DrawBarRow(dl, L, y, menu::g_Cursor == 2, ColorItems[2], p.hue,
                       [&](ImVec2 b0, ImVec2 b1) { DrawRainbow(dl, b0, b1); });                             y += L.rowH;
            DrawBarRow(dl, L, y, menu::g_Cursor == 3, ColorItems[3], (p.shade + 1.0f) * 0.5f,
                       [&](ImVec2 b0, ImVec2 b1) { DrawShadeBar(dl, b0, b1, p.hue); });                   y += L.rowH;

            /* プレビュー: NOW → NEW */
            teamcolor::Color4f* base = GetBaseColor(menu::g_Team);
            ImU32 nowCol = base ? ImGui::GetColorU32(ImVec4(base->r, base->g, base->b, 1.0f)) : bw::Gray;
            ImU32 newCol = ImGui::GetColorU32(ImVec4(o.color[0], o.color[1], o.color[2], 1.0f));
            float sz = 34 * unit;
            float cy = y + (previewH - sz) * 0.5f - 4 * unit;
            float cx = L.x + L.w * 0.5f;
            ImVec2 n0(cx - 90 * unit - sz * 0.5f, cy), w0(cx + 90 * unit - sz * 0.5f, cy);
            dl->AddRectFilled(n0, ImVec2(n0.x + sz, n0.y + sz), nowCol);
            dl->AddRect(n0, ImVec2(n0.x + sz, n0.y + sz), bw::White, 0, 0, 1.5f * unit);
            dl->AddRectFilled(w0, ImVec2(w0.x + sz, w0.y + sz), newCol);
            dl->AddRect(w0, ImVec2(w0.x + sz, w0.y + sz), bw::White, 0, 0, 1.5f * unit);
            float ls = 13 * unit;
            Text(dl, ls, ImVec2(n0.x + (sz - TextW(ls, "NOW")) * 0.5f, n0.y + sz + 4 * unit), bw::Gray, "NOW");
            Text(dl, ls, ImVec2(w0.x + (sz - TextW(ls, "NEW")) * 0.5f, w0.y + sz + 4 * unit), bw::Gray, "NEW");
            Text(dl, 18 * unit, ImVec2(cx - TextW(18 * unit, ">") * 0.5f, cy + sz * 0.5f - 9 * unit), bw::White, ">");
            y += previewH;
        }

        /* 下: 同期の進み具合 + 操作ガイド */
        float gs = 13 * unit;
        float gy = top + h - 22 * unit;
        if (sync::g_Active) {
            char sbuf[40];
            snprintf(sbuf, sizeof(sbuf), "SYNCING COLORS  %d%%", (int) (sync::Progress() * 100.0f));
            Text(dl, gs, ImVec2(L.x + L.pad, gy - 20 * unit), bw::White, sbuf);
        }
        const char* guide = colorPage ? "L SELECT   D-PAD ADJUST   L-STICK LEFT BACK" : "L SELECT   L-STICK LEFT CLOSE";
        Text(dl, gs, ImVec2(L.x + L.pad, gy), bw::Gray, guide);
    }

    void OnPreInit() {}

    void OnPostInit() {
        ImGui::GetIO().FontGlobalScale = 1.4f;
        intro::Restart();
    }

    void OnNewFrame() {
        ImGuiIO& io = ImGui::GetIO();
        ApplyTeamColors(io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : (1.0f / 60.0f));
    }

    void OnRender() {
        /* ロード画面の間はメニューを出さない */
        if (intro::Draw()) return;

        ImGuiIO& io = ImGui::GetIO();
        float dt = io.DeltaTime > 0.0f && io.DeltaTime < 0.25f ? io.DeltaTime : (1.0f / 60.0f);
        float unit = io.DisplaySize.y / 720.0f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        if (!IsMenuOpen()) {
            menu::g_WasOpen = false;
            DrawClosedTab(dl, unit);
            return;
        }

        if (!menu::g_WasOpen) {
            GoTo(Page::Main);
            menu::g_OpenTime = 0.0f;
            menu::g_WasOpen = true;
        }
        menu::g_OpenTime += dt;

        HandleInput(dt);
        if (IsMenuOpen()) DrawMenu(dl, unit);
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
