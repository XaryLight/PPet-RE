// PPet - C++ 移植版
// 轻量行为状态机：把散落的「触摸 / 拖拽 / 待机 / 挂起」触发统一成跨帧状态。
//
// 【为什么需要它】
//   现状是离散触发：待机是程序化循环，触摸是区域→动作映射，表情是预设切换。
//   没有跨帧状态决定「当前该做什么」以及「什么条件下转换」，于是：
//     - 触摸反应播完直接回待机，没有「被摸头后的余韵」；
//     - 无法表达「待机时被触摸 → 反应 A；播放动作时被触摸 → 反应 B」；
//     - 插件只能发通知，不能参与行为决策。
//   本模块提供最小可用抽象：状态 + 转换条件 + 输出（待机幅度 / 是否接受触摸）。
//
// 【参考设计】
//   - Desktop Homunculus 的 Behavior Service：状态 + 转换条件 + 动画输出；
//   - dsh-plugin-vrmpet3d 的状态联动：外部语义状态驱动行为选择。
//
// 【纯逻辑约束】只含 POD 与枚举，不碰 GL / Qt / 模型，可脱离后端单测。
#pragma once

#include <cstdint>

namespace ppet::model {

/// 桌宠行为状态。数值顺序即优先级（Suspended 最高）。
enum class BehaviorState : std::uint8_t {
    Idle = 0,       ///< 程序化待机
    Sitting,        ///< 窗口坐姿（预留：窗口几何接入后启用）
    Reacting,       ///< 正在播放触摸/情绪反应
    Interacting,    ///< 用户正在拖拽 / 持续交互
    Suspended,      ///< 挂起（全屏游戏 / 后台 / 安静模式）
};

/// 轻量行为状态机。
///
/// 不依赖行为树库，只覆盖桌宠实际需要的五类状态与转换。所有成员函数
/// 都是纯逻辑，可在无 GL / Qt 环境下单测。
class BehaviorStateMachine {
public:
    struct Config {
        /// 一次反应最长保持时间（秒）。即使骨骼动作已播完，也保留一段「余韵」。
        float reactionHoldSeconds = 1.6f;
        /// 挂起时待机幅度倍率（安静模式）。
        float suspendedIdleScale = 0.25f;
        /// 反应中待机幅度倍率（让位给动作，避免叠加抽搐）。
        float reactingIdleScale = 0.45f;
        /// 交互中待机幅度倍率。
        float interactingIdleScale = 0.70f;
    };

    BehaviorStateMachine() : config_(Config{}) {}
    explicit BehaviorStateMachine(Config config) : config_(config) {}

    void SetConfig(const Config& config) { config_ = config; }
    const Config& GetConfig() const { return config_; }

    // --- 事件（转换条件） ---

    /// 用户触摸。挂起时被忽略；已在 Reacting 时刷新余韵计时。
    void OnTouch() {
        if (state_ == BehaviorState::Suspended) return;
        TransitionTo(BehaviorState::Reacting);
    }

    /// 用户开始拖拽 / 持续交互。挂起时被忽略。
    void OnInteractionBegin() {
        if (state_ == BehaviorState::Suspended) return;
        TransitionTo(BehaviorState::Interacting);
    }

    /// 用户结束拖拽。
    void OnInteractionEnd() {
        if (state_ == BehaviorState::Interacting) TransitionTo(BehaviorState::Idle);
    }

    /// 反应播放完毕（由骨骼动作 IsPlaying() 判定，或由超时兜底）。
    void OnReactionFinished() {
        if (state_ == BehaviorState::Reacting) TransitionTo(BehaviorState::Idle);
    }

    /// 坐到窗口边缘（窗口坐姿行为，预留）。
    void OnSit() {
        if (state_ == BehaviorState::Suspended) return;
        TransitionTo(BehaviorState::Sitting);
    }

    /// 从窗口边缘站起。
    void OnStand() {
        if (state_ == BehaviorState::Sitting) TransitionTo(BehaviorState::Idle);
    }

    /// 进入挂起（最高优先级，可从任何状态进入）。
    void OnSuspend() { TransitionTo(BehaviorState::Suspended); }

    /// 退出挂起。
    void OnResume() {
        if (state_ == BehaviorState::Suspended) TransitionTo(BehaviorState::Idle);
    }

    /// 每帧推进。
    void Tick(float dt) {
        if (!(dt > 0.0f)) return;
        timeInState_ += dt;
        if (state_ == BehaviorState::Reacting) {
            reactionTimer_ -= dt;
            if (reactionTimer_ <= 0.0f) TransitionTo(BehaviorState::Idle);
        }
    }

    // --- 查询 / 输出 ---

    BehaviorState State() const { return state_; }
    float TimeInState() const { return timeInState_; }
    float ReactionTimeLeft() const { return reactionTimer_; }

    bool IsIdle() const { return state_ == BehaviorState::Idle; }
    bool IsReacting() const { return state_ == BehaviorState::Reacting; }
    bool IsSuspended() const { return state_ == BehaviorState::Suspended; }
    bool IsInteracting() const { return state_ == BehaviorState::Interacting; }

    /// 当前是否接受新的触摸（挂起时拒绝）。
    bool CanAcceptTouch() const { return state_ != BehaviorState::Suspended; }

    /// 当前应施加到程序化待机的幅度倍率。
    float IdleMasterScale() const {
        switch (state_) {
            case BehaviorState::Suspended:   return config_.suspendedIdleScale;
            case BehaviorState::Reacting:    return config_.reactingIdleScale;
            case BehaviorState::Interacting: return config_.interactingIdleScale;
            case BehaviorState::Sitting:     return 0.85f;
            case BehaviorState::Idle:
            default:                         return 1.0f;
        }
    }

    void Reset() {
        state_ = BehaviorState::Idle;
        timeInState_ = 0.0f;
        reactionTimer_ = 0.0f;
    }

private:
    void TransitionTo(BehaviorState next) {
        if (state_ == next) {
            // 同状态重入（连续触摸）：刷新余韵计时，但不重置驻留时间。
            if (next == BehaviorState::Reacting) {
                reactionTimer_ = config_.reactionHoldSeconds;
            }
            return;
        }
        state_ = next;
        timeInState_ = 0.0f;
        reactionTimer_ =
            (next == BehaviorState::Reacting) ? config_.reactionHoldSeconds : 0.0f;
    }

    Config config_;
    BehaviorState state_ = BehaviorState::Idle;
    float timeInState_ = 0.0f;
    float reactionTimer_ = 0.0f;
};

}  // namespace ppet::model
