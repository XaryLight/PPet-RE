// PPet - C++ 移植版
// 视线追踪策略：把「配置开关 → 使用哪种光标源 / 半径 / 偏转角上限」的判定
// 全部抽成纯函数，便于单元测试（App::BuildRenderContext 依赖窗口与追踪器，
// 难以直接单测），也让 App 只负责取数、不夹带业务判断。
//
// 设计要点（需求 7 修复）：
//   原实现里关闭「全屏追踪」后，BuildRenderContext 仍会 fallback 调用
//   GlobalCursorTracker::QueryCursor 拿到全局光标，于是模型照旧跟随整个屏幕的鼠标，
//   开关形同虚设。正确语义（对齐 AppConfig 注释）是：
//     - 全屏追踪开启：使用 OS 级全局光标（整个虚拟桌面坐标）。
//     - 全屏追踪关闭：只用「窗口内光标」（鼠标位于桌宠窗口内时跟随），
//       没有窗口内光标就不跟，绝不再去查全局坐标。
//
// Q5 追加修复：
//   App 曾用 `else if (hasWindowCursor_)` 绕过 None 判定，且未处理鼠标离开
//   （hasWindowCursor_ 永不复位），导致「关了全屏追踪仍在跟」。同时窗口内追踪
//   硬编码半径、PMX / VRM 未实现最大偏转角，使三个设置形同虚设。
#pragma once

#include <algorithm>

namespace ppet {

/// 本帧视线追踪应使用哪种光标源。
enum class TrackingSource {
    Global,  ///< 全屏追踪：OS 级全局光标（整个虚拟桌面）
    Window,  ///< 非全屏：仅窗口内光标（鼠标在桌宠窗口内）
    None,    ///< 没有任何可用光标源，不驱动视线（模型回正）
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

/// 有效的追踪半径（像素）：配置值非正时回退到 fallbackRadius，
/// 两者都无效时给 1.0f，避免归一化除零。
///
/// Q5：全屏追踪（Global）与窗口内追踪（Window）共用同一半径配置，
/// 不再让窗口分支硬编码「窗口半对角线」。
inline float EffectiveTrackingRadius(float configuredRadius, float fallbackRadius) {
    if (configuredRadius > 0.0f) return configuredRadius;
    if (fallbackRadius > 0.0f) return fallbackRadius;
    return 1.0f;
}

/// 把窗口内光标（窗口左上角为原点、y 向下）换算成 [-1, 1] 的视线方向
/// （x 右正、y 上正，与 RenderContext::localCursor / Global 分支一致）。
///
/// 用输出参数而非 Vec2，保持本头文件零依赖（不需要 include Math.hpp）。
inline void NormalizeWindowCursor(float cursorX, float cursorY, float windowWidth,
                                  float windowHeight, float radius, float* outX,
                                  float* outY) {
    const float r = radius > 0.0f ? radius : 1.0f;
    const float nx = (cursorX - windowWidth * 0.5f) / r;
    const float ny = -(cursorY - windowHeight * 0.5f) / r;
    if (outX) *outX = std::clamp(nx, -1.0f, 1.0f);
    if (outY) *outY = std::clamp(ny, -1.0f, 1.0f);
}

/// 视线角度上限（弧度）。
struct GazeLimits {
    float yawRad = 0.0f;
    float pitchRad = 0.0f;
};

/// 由配置的「最大偏转角（度）」推算头部 / 眼睛的 yaw / pitch 上限（弧度）。
///
/// @param maxAngleDegrees   配置值，内部钳制到 [0, 90]。
/// @param yawToPitchRatio   后端自身的 pitch:yaw 比例（<= 0 时回退 0.5）。
/// @param modelYawLimitRad  模型自身的 yaw 上限（弧度），> 0 时取较小值；
///        VRM 传 lookAt.rangeMap，PMX / Live2D 传 0 表示完全由配置决定。
///
/// Q5：Live2D 早已支持该配置，PMX / VRM 之前用硬编码常量，滑块无效。
inline GazeLimits ResolveGazeLimits(float maxAngleDegrees, float yawToPitchRatio,
                                    float modelYawLimitRad = 0.0f) {
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    float yaw = ClampTrackingAngle(maxAngleDegrees) * kDeg2Rad;
    if (modelYawLimitRad > 0.0f) yaw = std::min(yaw, modelYawLimitRad);
    const float ratio = yawToPitchRatio > 0.0f ? yawToPitchRatio : 0.5f;
    return {yaw, yaw * ratio};
}

}  // namespace ppet
