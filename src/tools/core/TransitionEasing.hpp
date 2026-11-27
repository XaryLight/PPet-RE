// PPet - C++ 移植版
// 动画过渡原语：统一的缓动曲线 + 权重守恒的交叉淡化（crossfade）。
//
// 【为什么要单独一层】（2026-11 网上调研的实现依据）
//   角色动画切换的「丝滑感」来自三件事：
//     1. 归一化进度 s ∈ [0,1]，过渡时长 d 通常取 0.1~0.5 秒；
//     2. 把线性进度过成一条「两头慢、中间快」的平滑曲线（端点导数为 0），
//        避免匀速切换的速度折角：
//          smoothstep: w = s²(3-2s)              （Unity / 通用动画实践）
//          EasingSine: w = 0.5 - 0.5·cos(πs)     （Cubism Framework 官方）
//     3. crossfade 时**新旧姿态按权重加权平均，权重之和为 1**，否则叠加后
//        姿态幅度会凭空放大或缩小（骨骼按名字累加时尤其明显，表现为「过冲」）。
//
//   本项目三套动画系统（Live2D MotionQueue / 骨骼 BoneMotionPlayer / 表情
//   ExpressionDirector）此前各自内联了 smoothstep 或 EasingSine，公式相同但
//   分散；且骨骼层的淡入淡出用两套独立计时器，权重不守恒。本模块把曲线与守恒
//   混合收敛为单一可单测的实现，各系统复用同一份规范。
//
// 【纯逻辑约束】零外部依赖、零 GL / Qt / 文件 IO，可脱离渲染上下文做单元测试。
#pragma once

#include <algorithm>
#include <cmath>

namespace ppet::transition {

/// 过渡缓动曲线族。两者端点导数为 0，区别只在中间段的加速度分布。
enum class Curve {
    SmoothStep,  ///< s²(3-2s)：Unity / 通用动画实践
    Sine,        ///< 0.5-0.5cos(πs)：Cubism Framework GetEasingSine
};

/// 把归一化进度 s 过成平滑曲线（s 先钳到 [0,1]）。
inline float Ease(float s, Curve curve = Curve::SmoothStep) {
    s = std::clamp(s, 0.0f, 1.0f);
    if (curve == Curve::Sine) {
        return 0.5f - 0.5f * std::cos(s * 3.14159265358979323846f);
    }
    return s * s * (3.0f - 2.0f * s);
}

/// 一次交叉淡化的两层权重。
struct CrossFadeWeights {
    float outgoing = 0.0f;  ///< 旧动作权重
    float incoming = 0.0f;  ///< 新动作权重
};

/// 计算权重守恒的交叉淡化权重对。
///
/// @param fromWeight 切换瞬间旧动作的权重 [0,1]（若旧动作正在淡出中被再次打断，
///                   这里 <1，避免凭空放大）
/// @param toWeight   新动作的目标权重 [0,1]（通常为 1）
/// @param progress   归一化进度 [0,1]，由调用方按 elapsed/duration 计算
/// @param curve      缓动曲线
/// @return outgoing = fromWeight·(1-Ease(p))，incoming = toWeight·Ease(p)。
///         当 fromWeight == toWeight == 1 时二者之和恒为 1（严格守恒）；
///         否则总和从 fromWeight 单调过渡到 toWeight，**中间不会超过两端最大值**。
inline CrossFadeWeights CrossFade(float fromWeight, float toWeight, float progress,
                                  Curve curve = Curve::SmoothStep) {
    const float e = Ease(progress, curve);
    CrossFadeWeights w;
    w.outgoing = fromWeight * (1.0f - e);
    w.incoming = toWeight * e;
    return w;
}

}  // namespace ppet::transition
