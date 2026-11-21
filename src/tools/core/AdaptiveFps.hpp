// PPet - 分场景自适应帧率（报告 4.1.3）
//
// 纯逻辑、零依赖（只依赖 Stability.hpp 的 EffectiveMaxFps），可单测。
// 设计取向「保守节能」：只在窗口明确不可见（最小化/托盘隐藏）时降到心跳帧率，
// 其余场景完全沿用既有配置帧率，避免误判导致桌宠动画卡顿——这是桌宠类程序
// 最不能接受的体验退化。窥屏/失焦降频需要可靠的遮挡检测（DWM CLOAKED 等），
// 跨平台语义不一，故不在此默认启用，留作可选扩展。
#pragma once

#include <src/tools/core/Stability.hpp>

namespace ppet::adaptive_fps {

/// 场景可见性（由调用方根据窗口状态给出）。
enum class Scene {
    Active,  ///< 窗口可见（正常渲染）
    Hidden,  ///< 窗口被最小化 / 隐藏（glfwGetWindowAttrib(GLFW_VISIBLE)==false）
};

/// 隐藏时的心跳帧率：维持最低限度的存活（响应托盘/退出），几乎不占 GPU。
constexpr int kHiddenHeartbeatFps = 1;

/// 计算目标帧率。
///   configuredFps：配置里的帧率上限（0=不限帧）
///   safeMode     ：兼容模式强制封顶 30
///   scene        ：当前场景
///   enabled      ：自适应开关；关闭时行为与既有 EffectiveMaxFps 完全一致
inline int EffectiveAdaptiveFps(int configuredFps, bool safeMode, Scene scene,
                                bool enabled) {
    if (!enabled) return stability::EffectiveMaxFps(configuredFps, safeMode);
    if (scene == Scene::Hidden) return kHiddenHeartbeatFps;
    return stability::EffectiveMaxFps(configuredFps, safeMode);
}

}  // namespace ppet::adaptive_fps
