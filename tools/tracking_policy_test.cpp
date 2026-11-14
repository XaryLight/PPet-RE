// PPet - C++ 移植版
// 视线追踪策略单元测试（需求 7）：
//   - 全屏追踪开启 -> 使用全局光标
//   - 全屏追踪关闭且有窗口内光标 -> 使用窗口内光标
//   - 全屏追踪关闭且无窗口内光标 -> 不追踪
//   - 最大偏转角度钳制到 [0, 90]
//
// 纯逻辑、零依赖（不碰窗口 / 追踪器），三种 CMake 配置均可编译运行。
#include <src/tools/core/TrackingPolicy.hpp>

#include <cassert>
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) {
            std::printf("FAIL: %s\n", msg);
            ++failures;
        }
    };

    using ppet::TrackingSource;
    using ppet::ResolveTrackingSource;
    using ppet::ClampTrackingAngle;

    // --- 全屏追踪开启：无论窗口内光标是否存在，都用全局光标 ---
    check(ResolveTrackingSource(true, false) == TrackingSource::Global,
          "全屏开启(无窗口光标) -> Global");
    check(ResolveTrackingSource(true, true) == TrackingSource::Global,
          "全屏开启(有窗口光标) -> Global");

    // --- 全屏追踪关闭：有窗口内光标用窗口，无则 None ---
    check(ResolveTrackingSource(false, true) == TrackingSource::Window,
          "全屏关闭(有窗口光标) -> Window");
    check(ResolveTrackingSource(false, false) == TrackingSource::None,
          "全屏关闭(无窗口光标) -> None");

    // --- 关键回归：关闭全屏后绝不再退化回全局光标 ---
    check(ResolveTrackingSource(false, false) != TrackingSource::Global,
          "需求7回归：关闭全屏后不应使用全局光标");

    // --- 最大偏转角度钳制 ---
    check(ClampTrackingAngle(30.0f) == 30.0f, "30 不变");
    check(ClampTrackingAngle(-10.0f) == 0.0f, "负数钳到 0");
    check(ClampTrackingAngle(120.0f) == 90.0f, "超 90 钳到 90");
    check(ClampTrackingAngle(0.0f) == 0.0f, "0 合法");
    check(ClampTrackingAngle(90.0f) == 90.0f, "90 合法");

    if (failures == 0) {
        std::printf("tracking_policy_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("tracking_policy_unit: %d FAILED\n", failures);
    return 1;
}
