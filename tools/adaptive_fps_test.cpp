// PPet - 分场景自适应帧率单测（报告 4.1.3）。纯逻辑、零 GUI 依赖。
#include <src/tools/core/AdaptiveFps.hpp>

#include <cstdio>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}
void CheckEq(int a, int b, const char* what) {
    if (a != b) {
        std::printf("[FAIL] %s（实际 %d，期望 %d）\n", what, a, b);
        ++g_failures;
    } else {
        std::printf("[ OK ] %s\n", what);
    }
}

using ppet::adaptive_fps::EffectiveAdaptiveFps;
using ppet::adaptive_fps::Scene;

}  // namespace

int main() {
    std::printf("== 分场景自适应帧率单测 ==\n");

    // 关闭自适应：与既有 EffectiveMaxFps 完全一致。
    CheckEq(EffectiveAdaptiveFps(60, false, Scene::Active, false), 60,
            "关闭时 Active 沿用配置 60");
    CheckEq(EffectiveAdaptiveFps(60, false, Scene::Hidden, false), 60,
            "关闭时 Hidden 仍沿用配置 60（不降频）");
    CheckEq(EffectiveAdaptiveFps(0, false, Scene::Active, false), 0,
            "关闭时 0=不限帧 保持 0");
    CheckEq(EffectiveAdaptiveFps(300, false, Scene::Active, false), 240,
            "关闭时超高值仍钳制到 240");

    // 开启自适应。
    CheckEq(EffectiveAdaptiveFps(60, false, Scene::Active, true), 60,
            "开启时 Active 保持 60");
    CheckEq(EffectiveAdaptiveFps(60, false, Scene::Hidden, true),
            ppet::adaptive_fps::kHiddenHeartbeatFps,
            "开启时 Hidden 降到心跳帧率");
    CheckEq(EffectiveAdaptiveFps(0, false, Scene::Hidden, true),
            ppet::adaptive_fps::kHiddenHeartbeatFps,
            "开启时 Hidden 即便配置不限帧也降心跳");

    // 安全模式：Active 封顶 30，Hidden 仍心跳 1。
    CheckEq(EffectiveAdaptiveFps(60, true, Scene::Active, true), 30,
            "安全模式 Active 封顶 30");
    CheckEq(EffectiveAdaptiveFps(60, true, Scene::Hidden, true), 1,
            "安全模式 Hidden 仍心跳 1");

    std::printf("\n结果：%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
