// PPet - C++ 移植版
// 视线追踪策略：把「配置开关 → 使用哪种光标源」的判定抽成纯函数，
// 便于单元测试（App::BuildRenderContext 依赖窗口与追踪器，难以直接单测）。
//
// 设计要点（需求 7 修复）：
//   原实现里关闭「全屏追踪」后，BuildRenderContext 仍会 fallback 调用
//   GlobalCursorTracker::QueryCursor 拿到全局光标，于是模型照旧跟随整个屏幕的鼠标，
//   开关形同虚设。正确语义（对齐 AppConfig 注释）是：
//     - 全屏追踪开启：使用 OS 级全局光标（整个虚拟桌面坐标）。
//     - 全屏追踪关闭：只用「窗口内光标」（鼠标位于桌宠窗口内时跟随），
//       没有窗口内光标就不跟，绝不再去查全局坐标。
#pragma once

#include <algorithm>

namespace ppet {

/// 本帧视线追踪应使用哪种光标源。
enum class TrackingSource {
    Global,  ///< 全屏追踪：OS 级全局光标（整个虚拟桌面）
    Window,  ///< 非全屏：仅窗口内光标（鼠标在桌宠窗口内）
    None,    ///< 没有任何可用光标源，不驱动视线
};

/// 由「全屏追踪开关」与「窗口内光标是否有效」决定本帧使用哪种光标源。
///
/// 纯函数，零依赖，可单测。
inline TrackingSource ResolveTrackingSource(bool fullscreenTracking,
                                           bool hasWindowCursor) {
    if (fullscreenTracking) return TrackingSource::Global;
    if (hasWindowCursor) return TrackingSource::Window;
    return TrackingSource::None;
}

/// 把配置里的「最大偏转角度」钳制到合法范围 [0, 90]（度）。
/// 与 AppConfig::Load 的校验范围一致，避免异常配置把模型拧到不自然姿势。
inline float ClampTrackingAngle(float degrees) {
    return std::clamp(degrees, 0.0f, 90.0f);
}

}  // namespace ppet
