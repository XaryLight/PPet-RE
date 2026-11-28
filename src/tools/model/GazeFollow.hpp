// PPet - C++ 移植版
// 凝视跟随（Gaze Follow）：把「光标目标」翻译成平滑、自然、可中断的头部/眼球朝向。
//
// 【参考来源】综合三个桌宠参考项目里工程化最成熟的凝视做法：
//   · dsh-plugin-vrmpet3d 的 Motion.applyGaze：
//       1) 凝视不是开关，而是「均匀 ramp」：gazeBlend 以 1/blendSeconds 的恒定速率
//          走向 0 或 1。停看与恢复都平滑转头，避免「瞬间回正 / 瞬间盯上」。
//       2) 鼠标静止休眠：光标连续 idleSeconds 没实质移动就停止凝视
//          （"如果鼠标连续5秒没有移动，就停止鼠标凝视"），epsilon 避免流式光标
//          重复同一坐标也一直续命。
//       3) 记住最后方向：清目标 / 被拒绝后沿最后方向淡出，不无方向跳回 0。
//       4) 头部预算让位（HEAD_LAYER_LIMIT）：内置动作也在转头时，凝视只用
//          「总预算 - 动作已转角度」的剩余额度，避免两套系统抢同一根骨骼。
//       5) 全程有限值防御：NaN/Inf 一旦写进骨骼四元数，模型会在两帧后整个消失。
//   · STVrmPet 的 updateHeadTrack：目标先做指数平滑（1-exp(-k·dt)），
//     再分摊到 spine / chest / neck / head，避免快速甩鼠标时头部瞬移。
//
// 【为什么是纯逻辑】不碰 GL / Qt / 文件 IO，只算「目标角度 -> 有效角度」，
//   可在无图形环境下单测；PMX / VRM 两个后端共用同一份凝视规范。
#pragma once

#include <algorithm>
#include <cmath>

namespace ppet::model {

/// 凝视跟随控制器。所有角度单位为弧度。
///
/// 用法（每帧）：
///   follow.SetAim(yaw, pitch);          // 上报本帧目标；无目标时 ClearAim
///   follow.SetMotionHeadTurn(rad);      // 内置动作占用了多少头部转角（可省略）
///   const GazeOutput out = follow.Tick(dt);
///   // 用 out.yaw / out.pitch 驱动头部与眼球骨骼
class GazeFollow {
public:
    /// 一帧凝视输出。
    struct Output {
        float yaw = 0.0f;    ///< 有效水平转角（已平滑、加权、ramp、受预算钳制）
        float pitch = 0.0f;  ///< 有效垂直转角
        float blend = 0.0f;  ///< 当前凝视混合系数 [0,1]（诊断用）
    };

    /// 凝视启用/停用缓冲时长（秒）。凝视起停都在这段时间内均匀过渡；
    /// 设为 0 表示瞬间切换（退回旧的硬行为）。默认 1.0 秒。
    void SetBlendSeconds(float seconds) {
        const float v = FiniteOr(seconds, 1.0f);
        blendSeconds_ = v >= 0.0f ? v : 1.0f;
    }
    float BlendSeconds() const { return blendSeconds_; }

    /// 鼠标静止多久（秒）后停止凝视。0 表示关闭休眠规则。默认 6 秒。
    void SetIdleSeconds(float seconds) {
        const float v = FiniteOr(seconds, 6.0f);
        idleSeconds_ = v >= 0.0f ? v : 6.0f;
    }
    float IdleSeconds() const { return idleSeconds_; }

    /// 判定「鼠标在动」的最小帧间变化量（弧度）。约 0.11°。默认 0.002。
    /// 只用于休眠计时，不限制目标本身（慢速移动也要能跟）。
    void SetMoveEpsilon(float epsilon) {
        const float v = FiniteOr(epsilon, 0.002f);
        moveEpsilon_ = v >= 0.0f ? v : 0.002f;
    }
    float MoveEpsilon() const { return moveEpsilon_; }

    /// 目标角度的指数平滑系数（每秒衰减到 1/e 的比例）。默认 9.0，
    /// 与旧实现的 kGazeSmoothing 一致；0 表示不平滑（直接跟随目标）。
    void SetFollowSmoothing(float coefficient) {
        const float v = FiniteOr(coefficient, 9.0f);
        followSmoothing_ = v >= 0.0f ? v : 9.0f;
    }
    float FollowSmoothing() const { return followSmoothing_; }

    /// 凝视相对骨骼静止姿态的权重。默认 0.85（与参考实现一致）。
    void SetWeight(float weight) {
        weight_ = Clamp(FiniteOr(weight, 0.85f), 0.0f, 1.0f);
    }
    float Weight() const { return weight_; }

    /// 水平凝视上限（弧度，正值）。默认 80°。
    void SetYawLimit(float radians) {
        const float v = FiniteOr(radians, 1.3962634f);
        yawLimit_ = v >= 0.0f ? v : 1.3962634f;
    }
    float YawLimit() const { return yawLimit_; }

    /// 垂直凝视上限：向上 / 向下分别设置（弧度，正值）。默认各 60°。
    void SetPitchLimits(float up, float down) {
        const float u = FiniteOr(up, 1.0471976f);
        const float d = FiniteOr(down, 1.0471976f);
        pitchUp_ = u >= 0.0f ? u : 1.0471976f;
        pitchDown_ = d >= 0.0f ? d : 1.0471976f;
    }

    /// 头部预算上限（弧度）：内置动作驱动头部时，凝视 + 动作的头部转角总和
    /// 不超过该值。默认 70°。设为 0 表示关闭预算让位。
    void SetHeadBudget(float radians) {
        const float v = FiniteOr(radians, 1.2217305f);
        headBudget_ = v >= 0.0f ? v : 1.2217305f;
    }
    float HeadBudget() const { return headBudget_; }

    /// 上报本帧内置动作已经占用的头部转角（弧度，取绝对值使用）。
    /// 每帧调用一次；不调用等价于 0。
    void SetMotionHeadTurn(float radians) {
        motionHeadTurn_ = std::fabs(FiniteOr(radians, 0.0f));
    }

    /// 上报本帧光标目标（弧度）。目标每帧都会更新（慢速移动也能跟），
    /// 但只有帧间变化超过 moveEpsilon 才刷新静止计时。
    /// NaN/Inf 会被忽略（保留上一次有效目标）。
    void SetAim(float yaw, float pitch) {
        if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;
        hasAim_ = true;
        hasEverAimed_ = true;
        const float dy = std::fabs(yaw - lastRawYaw_);
        const float dp = std::fabs(pitch - lastRawPitch_);
        if (dy > moveEpsilon_ || dp > moveEpsilon_) lastMoveAt_ = elapsed_;
        rawYaw_ = yaw;
        rawPitch_ = pitch;
        lastRawYaw_ = yaw;
        lastRawPitch_ = pitch;
    }

    /// 清除光标目标（光标离开 / 被预览 / 权限不足）。凝视将沿最后方向淡出。
    void ClearAim() { hasAim_ = false; }

    /// 是否正在跟随光标（供诊断 / UI 显示）。
    bool HasAim() const { return hasAim_; }

    /// 推进一帧并输出有效凝视角度。
    /// @param dt 帧间隔（秒）；非有限值或 <=0 时按 1/60 处理，>0.1 时钳到 0.1
    ///           （后台标签页恢复时可能给回几秒的 delta，钳制避免转头跳变）。
    Output Tick(float dt) {
        float step = std::isfinite(dt) ? dt : (1.0f / 60.0f);
        if (step <= 0.0f) step = 1.0f / 60.0f;
        if (step > 0.1f) step = 0.1f;
        elapsed_ += step;

        // --- 目标平滑（指数）：避免快速甩鼠标时头部瞬移 ---
        const float alpha = followSmoothing_ > 0.0f
                                ? 1.0f - std::exp(-followSmoothing_ * step)
                                : 1.0f;
        aimYaw_ += (rawYaw_ - aimYaw_) * alpha;
        aimPitch_ += (rawPitch_ - aimPitch_) * alpha;

        // --- 是否允许凝视：静止休眠 ---
        const bool idleRefused = idleSeconds_ > 0.0f &&
                                 (elapsed_ - lastMoveAt_) >= idleSeconds_;
        const bool allowed = hasAim_ && !idleRefused;

        // --- 均匀 ramp：gazeBlend 以恒定速率走向目标（0 或 1）---
        const float target = allowed ? 1.0f : 0.0f;
        if (!std::isfinite(blend_)) blend_ = target;
        if (blendSeconds_ <= 0.0f) {
            blend_ = target;
        } else {
            const float rate = step / blendSeconds_;
            const float remaining = target - blend_;
            blend_ += std::max(-rate, std::min(rate, remaining));
            blend_ = Clamp(blend_, 0.0f, 1.0f);
        }

        // 完全淡出，或从未有过目标：回到骨骼静止姿态（输出 0）。
        if (blend_ <= 0.0f || !hasEverAimed_) {
            return Output{0.0f, 0.0f, blend_};
        }

        // --- 角度钳制 + 权重 + ramp ---
        // 与参考实现一致：先算最终凝视贡献 = clamp(aim) * weight * blend，
        // 再用「头部预算」钳制这个最终值。预算是「动作 + 凝视的头部转角总和」上限，
        // 必须作用在加权之后，否则会重复压缩凝视幅度。
        float yaw = Clamp(aimYaw_, -yawLimit_, yawLimit_) * weight_ * blend_;
        float pitch = Clamp(aimPitch_, -pitchDown_, pitchUp_) * weight_ * blend_;

        if (headBudget_ > 0.0f) {
            const float budget = std::max(0.0f, headBudget_ - motionHeadTurn_);
            yaw = Clamp(yaw, -budget, budget);
            pitch = Clamp(pitch, -budget, budget);
        }

        // 末道有限值防线：任何坏值都不允许流向骨骼。
        if (!std::isfinite(yaw)) yaw = 0.0f;
        if (!std::isfinite(pitch)) pitch = 0.0f;
        return Output{yaw, pitch, blend_};
    }

    /// 当前凝视混合系数（诊断用）。
    float Blend() const { return blend_; }

    /// 复位到初始状态（切换模型 / 重新加载时调用）。
    void Reset() {
        hasAim_ = false;
        hasEverAimed_ = false;
        blend_ = 0.0f;
        elapsed_ = 0.0f;
        lastMoveAt_ = 0.0f;
        rawYaw_ = rawPitch_ = 0.0f;
        lastRawYaw_ = lastRawPitch_ = 0.0f;
        aimYaw_ = aimPitch_ = 0.0f;
        motionHeadTurn_ = 0.0f;
    }

private:
    static float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
    static float FiniteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

    float blendSeconds_ = 1.0f;
    float idleSeconds_ = 6.0f;
    float moveEpsilon_ = 0.002f;
    float followSmoothing_ = 9.0f;
    float weight_ = 0.85f;
    float yawLimit_ = 1.3962634f;     // 80°
    float pitchUp_ = 1.0471976f;      // 60°
    float pitchDown_ = 1.0471976f;    // 60°
    float headBudget_ = 1.2217305f;   // 70°

    bool hasAim_ = false;
    bool hasEverAimed_ = false;
    float rawYaw_ = 0.0f;
    float rawPitch_ = 0.0f;
    float lastRawYaw_ = 0.0f;
    float lastRawPitch_ = 0.0f;
    float aimYaw_ = 0.0f;
    float aimPitch_ = 0.0f;
    float motionHeadTurn_ = 0.0f;

    float blend_ = 0.0f;
    float elapsed_ = 0.0f;
    float lastMoveAt_ = 0.0f;
};

}  // namespace ppet::model
