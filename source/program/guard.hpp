// guard.hpp — オフライン判定
// 許可リスト方式: ヒーローモードかサイドオーダーのときだけ true。
// それ以外(バトル・サーモンラン・街・ロビー・未知のシーン)はすべて false。
#pragma once
#include "game.hpp"

namespace nn::nifm {
    bool IsNetworkAvailable();
}

namespace chroma::guard {

    // シーンだけを見る判定(プローブの記録はこれで制御する)
    inline bool IsOfflineScene() {
        using namespace game::flag;
        if (game::ReadFlag(IsVersus) || game::ReadFlag(IsVersusRc) || game::ReadFlag(IsCoop) ||
            game::ReadFlag(IsAroundPlaza) || game::ReadFlag(IsPlazaRc))
            return false;
        return game::ReadFlag(IsMission) || game::ReadFlag(IsSdodr);
    }

    // 機能を有効にしてよいか: オフラインのシーン かつ インターネット未接続
    // (機内モード推奨。nifm はゲーム起動時に初期化済みなので、シーンに入ったあとなら安全に呼べる)
    inline bool FeaturesAllowed() {
        if (!IsOfflineScene()) return false;
        return !nn::nifm::IsNetworkAvailable();
    }
}
