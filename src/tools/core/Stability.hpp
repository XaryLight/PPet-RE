// PPet - C++ 移植版
// 长期运行稳定性小工具（纯头文件、零依赖、可单测）。
//
// 桌宠是「开机就挂着、连续跑几周」的程序，很多在短跑里看不出的问题会慢慢浮现：
//   · 帧计数 / 序号用无符号回绕，回绕瞬间差值变成天文数字，触发误判；
//   · 用 float 累加时间，跑几天后有效位数不够，正弦相位一格一格跳；
//   · 窗口被挂起后 dt 变成极大值或 NaN，姿态/物理求解直接炸掉。
// 把这些「边界算术」集中到这里，避免每处各写一套。
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace ppet::stability {

/// 饱和递增：到上限后停住，绝不回绕成 0。
/// uint64_t 在 60fps 下要 97 亿年才满，但显式饱和让「计数器语义」可被验证，
/// 也防止将来有人把类型换成 32 位。
inline std::uint64_t SaturatingIncrement(std::uint64_t value) {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
}

/// 安全的无符号差值：now < then（计数器被重置 / 回绕 / 配置热重载）时返回 0，
/// 而不是下溢成 2^64-1 让「每 N 帧自检」这类判断永远为真。
inline std::uint64_t SafeDelta(std::uint64_t now, std::uint64_t then) {
    return now >= then ? now - then : 0;
}

/// 钳制帧间隔：负数、NaN、Inf 与「切后台回来」的超大 dt 一律收敛到 [0, maxDt]。
inline float ClampDelta(float dt, float maxDt = 0.1f) {
    if (!std::isfinite(dt) || dt <= 0.0f) return 0.0f;
    return dt > maxDt ? maxDt : dt;
}

/// 用 double 计算并归约正弦相位，返回 [0, 2π) 的 float。
/// 适用于「单条正弦」；若还要派生 sin(phase * k)，请直接用 double 相位
/// （见 IdleAnimator.hpp 的说明），否则归约点会让派生项跳变。
inline float PhaseOf(double elapsedSeconds, double frequencyHz, double offset = 0.0) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double phase = elapsedSeconds * frequencyHz * kTwoPi + offset;
    return static_cast<float>(std::fmod(phase, kTwoPi));
}

/// 计算本帧允许的最大帧率：安全模式强制 30；0 表示不额外限帧（只依赖 VSync）；
/// 负值与异常高值收敛到 [0, 240]，避免坏配置把主循环压成忙等或无响应。
inline int EffectiveMaxFps(int configuredFps, bool safeMode) {
    if (safeMode) return 30;
    if (configuredFps <= 0) return 0;
    if (configuredFps > 240) return 240;
    return configuredFps;
}

/// 帧预算（秒）：effectiveFps <= 0 时返回 0，调用方跳过软限帧。
inline double FrameBudgetSeconds(int effectiveFps) {
    return effectiveFps > 0 ? 1.0 / static_cast<double>(effectiveFps) : 0.0;
}

}  // namespace ppet::stability
