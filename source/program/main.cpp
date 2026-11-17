#include "lib.hpp"

/*
 * 動作確認用の最小構成。
 * フックは何も掛けず、exlaunch がゲームに読み込まれて普通に起動できるかだけを確かめる。
 * これで起動できたら、次の段階でメニュー(ImGui)やフックを足していく。
 */

extern "C" void exl_main(void* x0, void* x1) {
    /* Setup hooking environment. */
    exl::hook::Initialize();
}

extern "C" NORETURN void exl_exception_entry() {
    /* Note: this is only applicable in the context of applets/sysmodules. */
    EXL_ABORT("Default exception handler called!");
}
