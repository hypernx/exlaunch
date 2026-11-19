// probe.cpp — 実機で「毎フレーム呼ばれる関数」と「オブジェクトの中身」を記録する
//
// しくみ:
//   1. 敵・プレイヤー部品の vtable にある関数の入口にインラインフックを置く
//      (全レジスタを保存・復元するので、ゲームの動作には影響しない)
//   2. 呼ばれた回数を数え、this(x0)の vtable が対象クラスと一致すればアドレスを覚える
//   3. 別スレッドが 0.5 秒ごとに、回数とオブジェクトのメモリを SD に書き出す
//      sd:/ChromaRush/probe.bin   … 解析用データ
//      sd:/ChromaRush/status.txt  … 記録できているかの確認用
//   記録する条件: ヒーローモード / サイドオーダーにいる、または インターネット未接続(機内モード)。
//   機内モードなら試し打ち場(ロビー)でも記録する。プローブは記録だけで、ゲームの動作は変えない。
#include "lib.hpp"
#include "nn.hpp"
#include "lib/nx/nx.h"
#include "game.hpp"
#include "guard.hpp"
#include "recorder.hpp"
#include "storage.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <utility>

namespace nn::fs {
    Result DeleteFile(char const* path);
}

namespace chroma::recorder {
namespace {

    struct ClassDesc { const char* name; uintptr_t vt; u32 size; };
    constexpr ClassDesc kClasses[] = {
#define PROBE_CLASS(n, v, s) { n, v, s },
#include "recorder_classes.inc"
#undef PROBE_CLASS
    };
    constexpr int kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);

    struct TargetDesc { u8 cls; u8 kind; u16 slot; uintptr_t off; };
    constexpr TargetDesc kTargets[] = {
#define PROBE_TARGET(c, k, s, o) { c, k, s, o },
#include "recorder_targets.inc"
#undef PROBE_TARGET
    };
    constexpr int kNumTargets = sizeof(kTargets) / sizeof(kTargets[0]);

    constexpr int kMaxInst      = 3;          // クラスごとに覚えるインスタンス数
    constexpr u32 kMaxDump      = 0x1400;     // 1オブジェクトの最大ダンプサイズ
    constexpr int kFollowQwords = 0x10;       // プレイヤー部品: 先頭 0x80 バイト内のポインタを追う
    constexpr u32 kFollowSize   = 0x100;
    constexpr int kTickMs       = 500;
    constexpr uintptr_t kFlagRegionStart = 0x60f64c0;  // IsVersus(0x60f64d8)〜Sdodr系(0x60f658c)を含む
    constexpr u32       kFlagRegionSize  = 0x100;
    constexpr s64 kMaxFileSize  = 32 * 1024 * 1024;
    constexpr const char* kDir    = "sd:/ChromaRush";
    constexpr const char* kBin    = "sd:/ChromaRush/probe.bin";
    constexpr const char* kStatus = "sd:/ChromaRush/status.txt";

    std::atomic<bool>      g_recording{false};
    std::atomic<u32>       g_counts[kNumTargets];
    std::atomic<uintptr_t> g_inst[kNumClasses][kMaxInst];

    // ---- フック側(ゲームのスレッドで動く。軽く・ロックなしで) ----
    void OnHit(int idx, uintptr_t self) {
        if (!g_recording.load(std::memory_order_relaxed)) return;
        g_counts[idx].fetch_add(1, std::memory_order_relaxed);

        if (self == 0 || (self & 7) != 0) return;
        const TargetDesc& t = kTargets[idx];
        // x0 はメンバ関数の this。vtable が一致したものだけ記録する
        if (*reinterpret_cast<uintptr_t*>(self) != chroma::game::Addr(kClasses[t.cls].vt)) return;

        auto& slots = g_inst[t.cls];
        for (int i = 0; i < kMaxInst; i++) {
            uintptr_t cur = slots[i].load(std::memory_order_relaxed);
            if (cur == self) return;
            if (cur == 0) {
                uintptr_t expected = 0;
                if (slots[i].compare_exchange_strong(expected, self)) return;
                if (expected == self) return;
            }
        }
    }

    template<int I>
    struct RecorderHook : public ::exl::hook::impl::InlineHook<RecorderHook<I>> {
        static void Callback(exl::hook::InlineFloatCtx* ctx) {
            OnHit(I, ctx->X[0]);
        }
    };

    template<int... Is>
    void InstallAll(std::integer_sequence<int, Is...>) {
        (RecorderHook<Is>::InstallAtOffset(kTargets[Is].off), ...);
    }

    // ---- 記録スレッド側 ----
    bool IsReadable(uintptr_t addr, u64 len) {
        if (addr == 0) return false;
        MemoryInfo mi{}; u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return false;
        if ((mi.perm & Perm_R) == 0 || mi.type == 0) return false;
        return addr + len <= mi.addr + mi.size;
    }

    nn::fs::FileHandle g_file{};
    s64 g_pos = 0;
    bool g_fileOk = false;

    void Put(const void* p, u64 n) {
        if (!g_fileOk || g_pos + (s64)n > kMaxFileSize) return;
        nn::fs::WriteFile(g_file, g_pos, p, n, nn::fs::WriteOption::CreateOption(0));
        g_pos += n;
    }
    template<typename T> void PutV(const T& v) { Put(&v, sizeof(T)); }

    alignas(16) u8 g_buf[kMaxDump];

    void WriteStatus(u32 ticks, const char* note) {
        char text[256];
        int n = std::snprintf(text, sizeof(text),
            "ChromaRush probe\nticks=%u\nbytes=%ld\nrecording=%d\nhooks=%d\n%s\n",
            ticks, (long)g_pos, (int)g_recording.load(), kNumTargets, note);
        nn::fs::DeleteFile(kStatus);
        if (R_FAILED(nn::fs::CreateFile(kStatus, n))) return;
        nn::fs::FileHandle h;
        if (R_FAILED(nn::fs::OpenFile(&h, kStatus, nn::fs::OpenMode_Write))) return;
        nn::fs::WriteFile(h, 0, text, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
        nn::fs::CloseFile(h);
    }

    void WriteHeader() {
        Put("CRPB", 4);
        PutV<u32>(2);                      // フォーマットのバージョン
        PutV<u32>(kNumTargets);
        PutV<u32>(kNumClasses);
        PutV<u64>(exl::util::modules::GetTargetStart());
        for (auto& t : kTargets) { PutV<u8>(t.cls); PutV<u8>(t.kind); PutV<u16>(t.slot); PutV<u64>(t.off); }
        for (auto& c : kClasses) { PutV<u64>(c.vt); PutV<u32>(c.size); }
    }

    void WriteTick(u32 tick) {
        Put("TICK", 4);
        PutV<u32>(tick);
        PutV<u64>(nn::os::GetSystemTick().GetInt64Value());
        using namespace chroma::game::flag;
        u8 flags[8] = {
            (u8)game::ReadFlag(IsMission), (u8)game::ReadFlag(IsSdodr), (u8)game::ReadFlag(IsVersus),
            (u8)game::ReadFlag(IsCoop), (u8)game::ReadFlag(IsAroundPlaza), 0, 0, 0 };
        Put(flags, sizeof(flags));
        // シーンフラグの領域をまるごと記録(試し打ち場のフラグを特定するため)
        Put(reinterpret_cast<const void*>(chroma::game::Addr(kFlagRegionStart)), kFlagRegionSize);
        for (int i = 0; i < kNumTargets; i++) PutV<u32>(g_counts[i].exchange(0, std::memory_order_relaxed));

        for (int c = 0; c < kNumClasses; c++) {
            const u32 size = kClasses[c].size < kMaxDump ? kClasses[c].size : kMaxDump;
            const uintptr_t vt = chroma::game::Addr(kClasses[c].vt);
            const bool isPlayer = kClasses[c].name[0] == 'P';
            for (int i = 0; i < kMaxInst; i++) {
                uintptr_t obj = g_inst[c][i].load(std::memory_order_relaxed);
                if (obj == 0) continue;
                // 解放済みや別物になっていたら捨てる
                if (!IsReadable(obj, size) || *reinterpret_cast<uintptr_t*>(obj) != vt) {
                    g_inst[c][i].store(0, std::memory_order_relaxed);
                    continue;
                }
                std::memcpy(g_buf, reinterpret_cast<void*>(obj), size);
                Put("OBJ ", 4);
                PutV<u8>(c); PutV<u8>(i); PutV<u16>(0);
                PutV<u64>(obj);
                PutV<u32>(size);
                Put(g_buf, size);

                if (!isPlayer) continue;
                // プレイヤー部品は、先頭付近のポインタの先も少しだけ記録する(位置などが別オブジェクトにある可能性)
                for (int q = 1; q < kFollowQwords && (u32)(q * 8 + 8) <= size; q++) {
                    uintptr_t p;
                    std::memcpy(&p, g_buf + q * 8, 8);
                    if ((p & 7) != 0 || chroma::game::IsInMain(p) || !IsReadable(p, kFollowSize)) continue;
                    Put("PTR ", 4);
                    PutV<u8>(c); PutV<u8>(i); PutV<u16>(q * 8);
                    PutV<u64>(p);
                    std::memcpy(g_buf, reinterpret_cast<void*>(p), kFollowSize);
                    Put(g_buf, kFollowSize);
                    std::memcpy(g_buf, reinterpret_cast<void*>(obj), size);   // 元に戻す
                }
            }
        }
        nn::fs::FlushFile(g_file);
    }

    alignas(0x1000) u8 g_stack[0x8000];
    nn::os::ThreadType g_thread;

    void ThreadMain(void*) {
        // ゲームの初期化が終わるまで待つ
        nn::os::SleepThread(nn::TimeSpan::FromSeconds(10));

        if (!chroma::storage::EnsureSdMounted()) return;
        nn::fs::CreateDirectory(kDir);
        nn::fs::DeleteFile(kBin);
        if (R_SUCCEEDED(nn::fs::CreateFile(kBin, 0)) &&
            R_SUCCEEDED(nn::fs::OpenFile(&g_file, kBin, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) {
            g_fileOk = true;
            WriteHeader();
            nn::fs::FlushFile(g_file);
        }
        WriteStatus(0, g_fileOk ? "waiting (Hero Mode / Side Order / airplane mode)" : "ERROR: cannot create probe.bin");
        if (!g_fileOk) return;

        u32 tick = 0;
        while (g_pos < kMaxFileSize) {
            nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(kTickMs));
            const bool rec = chroma::guard::IsOfflineScene() || !nn::nifm::IsNetworkAvailable();
            g_recording.store(rec, std::memory_order_relaxed);
            if (!rec) {
                // シーンを出たらインスタンスを忘れる(次のステージで取り直す)
                for (auto& cls : g_inst) for (auto& s : cls) s.store(0, std::memory_order_relaxed);
                continue;
            }
            WriteTick(tick++);
            if (tick % 10 == 0) WriteStatus(tick, "recording");
        }
        g_recording.store(false);
        WriteStatus(tick, "finished (file size limit reached)");
        nn::fs::CloseFile(g_file);
    }
}

    void Install() {
        InstallAll(std::make_integer_sequence<int, kNumTargets>{});
        nn::os::CreateThread(&g_thread, ThreadMain, nullptr, g_stack, sizeof(g_stack), 20);
        nn::os::StartThread(&g_thread);
    }
}
