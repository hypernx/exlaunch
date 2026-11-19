// game.hpp — Splatoon 3 の main に関する確定済みオフセット
// 対象 main の Build ID: 28c4287aee36f7499da60f3e68b54c70da382d75
// 値はすべて main モジュール先頭からのオフセット。
#pragma once
#include "lib.hpp"

namespace chroma::game {

    // 0x3595c00 が今のシーンの属性を読み、以下の bool(1バイト)に書き込む [確定]
    namespace flag {
        constexpr uintptr_t IsVersus      = 0x60f64d8;  // バトル
        constexpr uintptr_t IsMission     = 0x60f64f8;  // ヒーローモード
        constexpr uintptr_t IsCoop        = 0x60f6518;  // サーモンラン
        constexpr uintptr_t IsAroundPlaza = 0x60f6538;  // 街
        constexpr uintptr_t IsVersusRc    = 0x60f6544;
        constexpr uintptr_t IsPlazaRc     = 0x60f6554;
        constexpr uintptr_t IsSdodr       = 0x60f6580;  // サイドオーダー
    }

    inline uintptr_t Addr(uintptr_t off) { return exl::util::modules::GetTargetOffset(off); }

    inline bool ReadFlag(uintptr_t off) {
        return *reinterpret_cast<volatile u8*>(Addr(off)) != 0;
    }

    // main の .bss や vtable を指しているかの判定に使う
    inline bool IsInMain(uintptr_t p) {
        const auto& r = exl::util::GetMainModuleInfo().m_Total;
        return p >= r.m_Start && p < r.m_Start + r.m_Size;
    }
}
