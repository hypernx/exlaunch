// storage.hpp — SD カードを一度だけマウントする(記録機能と設定の保存で共有)
#pragma once
#include "lib.hpp"
#include "nn.hpp"
#include <atomic>

namespace nn::fs {
    Result DeleteFile(char const* path);
}

namespace chroma::storage {

    /* 0 = まだ, 1 = マウント中, 2 = 成功, 3 = 失敗 */
    inline std::atomic<int> g_MountState{0};

    inline bool EnsureSdMounted() {
        int s = g_MountState.load();
        if (s == 2) return true;
        if (s == 3) return false;
        int expected = 0;
        if (g_MountState.compare_exchange_strong(expected, 1)) {
            bool ok = R_SUCCEEDED(nn::fs::MountSdCardForDebug("sd"));
            if (ok) nn::fs::CreateDirectory("sd:/ChromaRush");
            g_MountState.store(ok ? 2 : 3);
            return ok;
        }
        /* ほかのスレッドがマウント中なら終わるまで待つ */
        while (g_MountState.load() == 1) nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(5));
        return g_MountState.load() == 2;
    }
}
