// PPet - C++ 移植版
// 轻量行为状态机：把散落的「触摸 / 拖拽 / 表情 / 说话 / 挂起 / 窗口空间」
// 触发统一收敛成跨帧状态，作为所有行为的唯一仲裁点。
//
// 【为什么需要它】
//   早期实现是离散触发：待机是程序化循环，触摸是区域→动作映射，表情是预设
//   切换，说话是独立的口型层。它们各自为政，无法表达「待机时被触摸 → 反应 A；
//   播放动作时被触摸 → 反应 B」这类优先级逻辑。本模块把触发源统一成
//   BehaviorEvent，内部维护一张「当前状态 × 事件 → 下一状态」的转换表，
//   并对外输出当前状态下的行为指令（待机幅度倍率 / 是否接受触摸）。
//
// 【参考设计】
//   - Desktop Homunculus 的 Behavior Service：状态 + 转换条件 + 动画输出；
//   - dsh-plugin-vrmpet3d 的状态联动：外部语义状态驱动行为选择。
//
// 【纯逻辑约束】只含 POD 与枚举，不碰 GL / Qt / 模型，可脱离后端单测。
#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ppet::model {

/// 桌宠行为状态。
enum class BehaviorState : std::uint8_t {
    Idle = 0,       ///< 程序化待机（+ 可选 VRMA 待机循环）
    Sitting,        ///< 窗口坐姿（窗口几何接入后启用）
    Talking,        ///< 气泡说话期间（口型驱动中）
    Reacting,       ///< 正在播放触摸 / 情绪反应
    Interacting,    ///< 用户正在拖拽 / 持续交互
    Suspended,      ///< 挂起（全屏游戏 / 后台 / 插件请求静默）
};

/// 内置状态优先级（数值越大越优先）。externalOverride 与此比较后仲裁。
inline int BehaviorStatePriority(BehaviorState state) {
    switch (state) {
        case BehaviorState::Idle:        return 0;
        case BehaviorState::Sitting:     return 1;
        case BehaviorState::Talking:     return 2;
        case BehaviorState::Reacting:    return 3;
        case BehaviorState::Interacting: return 4;
        case BehaviorState::Suspended:   return 100;
    }
    return 0;
}

/// 统一的行为触发源（收敛到状态机的唯一入口）。
enum class BehaviorEvent : std::uint8_t {
    TouchStart,        ///< 用户触摸 / 点击开始
    TouchEnd,          ///< 触摸反应播放完毕
    InteractionBegin,  ///< 用户开始拖拽
    InteractionEnd,    ///< 用户结束拖拽
    ExpressionChanged, ///< 表情预设切换
    PluginStateChanged,///< 插件请求状态覆盖（经 externalOverride 仲裁）
    WindowDetached,    ///< 窗口坐姿目标丢失（窗口关闭 / 最小化）
    SpeechStarted,     ///< 开始说话（气泡 + 口型）
    SpeechEnded,       ///< 结束说话
    Sit,               ///< 坐到窗口边缘
    Stand,             ///< 从窗口边缘站起
    Suspend,           ///< 进入挂起
    Resume,            ///< 退出挂起
};

/// 插件 / 外部请求的临时状态覆盖（带优先级，与内置状态每帧仲裁）。
struct ExternalStateOverride {
    BehaviorState state = BehaviorState::Idle;
    int priority = 0;  ///< 0 表示未激活
};

/// 轻量行为状态机。
///
/// 不依赖行为树库，只覆盖桌宠实际需要的六类状态与转换。所有成员函数
/// 都是纯逻辑，可在无 GL / Qt 环境下单测。
class BehaviorStateMachine {
public:
    struct Config {
        /// 一次触摸反应最长保持时间（秒）。即使骨骼动作已播完，也保留一段「余韵」。
        float reactionHoldSeconds = 1.6f;
        /// 表情切换作为即时反馈时的短余韵（秒）。
        float expressionHoldSeconds = 0.8f;
        /// 挂起时待机幅度倍率（安静模式）。
        float suspendedIdleScale = 0.25f;
        /// 反应中待机幅度倍率（让位给动作，避免叠加抽搐）。
        float reactingIdleScale = 0.45f;
        /// 交互中待机幅度倍率。
        float interactingIdleScale = 0.70f;
        /// 说话中待机幅度倍率（口型驱动，身体动作略收）。
        float talkingIdleScale = 0.80f;
    };

    BehaviorStateMachine() : config_(Config{}) {}
    explicit BehaviorStateMachine(Config config) : config_(config) {}

    void SetConfig(const Config& config) { config_ = config; }
    const Config& GetConfig() const { return config_; }

    // --- 事件入口（统一收敛点） ---

    /// 处理单个事件。
    void ProcessEvent(BehaviorEvent event) {
        switch (event) {
            case BehaviorEvent::TouchStart: OnTouch(); break;
            case BehaviorEvent::TouchEnd: OnReactionFinished(); break;
            case BehaviorEvent::InteractionBegin: OnInteractionBegin(); break;
            case BehaviorEvent::InteractionEnd: OnInteractionEnd(); break;
            case BehaviorEvent::ExpressionChanged:
                // 表情切换也是一种即时反馈：非高优先级状态下短暂进入 Reacting。
                if (state_ != BehaviorState::Suspended &&
                    state_ != BehaviorState::Interacting &&
                    state_ != BehaviorState::Reacting) {
                    TransitionTo(BehaviorState::Reacting, config_.expressionHoldSeconds);
                }
                break;
            case BehaviorEvent::PluginStateChanged:
                // 覆盖请求已通过 SetExternalOverride 写入，仲裁在 EffectiveState 里做。
                break;
            case BehaviorEvent::WindowDetached:
                if (state_ == BehaviorState::Sitting) TransitionTo(BehaviorState::Idle);
                break;
            case BehaviorEvent::SpeechStarted:
                // 说话不打打断更优先的交互 / 反应，仅从 Idle/Sitting/Talking 进入。
                if (state_ != BehaviorState::Suspended &&
                    state_ != BehaviorState::Interacting &&
                    state_ != BehaviorState::Reacting) {
                    TransitionTo(BehaviorState::Talking);
                }
                break;
            case BehaviorEvent::SpeechEnded:
                if (state_ == BehaviorState::Talking) TransitionTo(BehaviorState::Idle);
                break;
            case BehaviorEvent::Sit: OnSit(); break;
            case BehaviorEvent::Stand: OnStand(); break;
            case BehaviorEvent::Suspend: OnSuspend(); break;
            case BehaviorEvent::Resume: OnResume(); break;
        }
    }

    /// 批量处理事件（一帧内可能同时发生多个触发源）。
    void ProcessEvents(std::initializer_list<BehaviorEvent> events) {
        for (BehaviorEvent e : events) ProcessEvent(e);
    }
    void ProcessEvents(const std::vector<BehaviorEvent>& events) {
        for (BehaviorEvent e : events) ProcessEvent(e);
    }

    // --- 便捷方法（内部仍走 ProcessEvent 语义，保持调用点简洁） ---

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

    // --- 外部覆盖（插件行为契约，缺陷 5 的接入点） ---

    /// 设置一个带优先级的临时状态覆盖（插件 / 外部语义状态）。
    void SetExternalOverride(BehaviorState state, int priority) {
        externalOverride_.state = state;
        externalOverride_.priority = std::max(0, priority);
    }

    /// 清除外部覆盖。
    void ClearExternalOverride() { externalOverride_ = ExternalStateOverride{}; }

    /// 当前外部覆盖（未激活时 priority == 0）。
    const ExternalStateOverride& ExternalOverride() const { return externalOverride_; }

    // --- 每帧推进 ---

    void Tick(float dt) {
        if (!(dt > 0.0f)) return;
        timeInState_ += dt;
        if (state_ == BehaviorState::Reacting) {
            reactionTimer_ -= dt;
            if (reactionTimer_ <= 0.0f) TransitionTo(BehaviorState::Idle);
        }
    }

    // --- 查询 / 输出（均基于「有效状态」= 内置状态与外部覆盖仲裁后的结果） ---

    /// 内置状态（不含外部覆盖）。
    BehaviorState InternalState() const { return state_; }

    /// 有效状态：外部覆盖优先级不低于内置状态时，返回覆盖状态。
    BehaviorState State() const { return EffectiveState(); }

    float TimeInState() const { return timeInState_; }
    float ReactionTimeLeft() const { return reactionTimer_; }

    bool IsIdle() const { return EffectiveState() == BehaviorState::Idle; }
    bool IsReacting() const { return EffectiveState() == BehaviorState::Reacting; }
    bool IsSuspended() const { return EffectiveState() == BehaviorState::Suspended; }
    bool IsInteracting() const { return EffectiveState() == BehaviorState::Interacting; }
    bool IsTalking() const { return EffectiveState() == BehaviorState::Talking; }

    /// 当前是否接受新的触摸（挂起时拒绝）。
    bool CanAcceptTouch() const { return EffectiveState() != BehaviorState::Suspended; }

    /// 当前应施加到程序化待机的幅度倍率。
    float IdleMasterScale() const {
        switch (EffectiveState()) {
            case BehaviorState::Suspended:   return config_.suspendedIdleScale;
            case BehaviorState::Reacting:    return config_.reactingIdleScale;
            case BehaviorState::Interacting: return config_.interactingIdleScale;
            case BehaviorState::Talking:     return config_.talkingIdleScale;
            case BehaviorState::Sitting:     return 0.85f;
            case BehaviorState::Idle:
            default:                         return 1.0f;
        }
    }

    void Reset() {
        state_ = BehaviorState::Idle;
        timeInState_ = 0.0f;
        reactionTimer_ = 0.0f;
        externalOverride_ = ExternalStateOverride{};
    }

private:
    BehaviorState EffectiveState() const {
        if (externalOverride_.priority > 0 &&
            externalOverride_.priority >= BehaviorStatePriority(state_)) {
            return externalOverride_.state;
        }
        return state_;
    }

    /// @param reactionHold 进入 Reacting 时的余韵时长；负数表示用配置默认值。
    void TransitionTo(BehaviorState next, float reactionHold = -1.0f) {
        if (state_ == next) {
            // 同状态重入（连续触摸）：刷新余韵计时，但不重置驻留时间。
            if (next == BehaviorState::Reacting) {
                reactionTimer_ =
                    (reactionHold >= 0.0f) ? reactionHold : config_.reactionHoldSeconds;
            }
            return;
        }
        state_ = next;
        timeInState_ = 0.0f;
        reactionTimer_ = (next == BehaviorState::Reacting)
                             ? ((reactionHold >= 0.0f) ? reactionHold
                                                       : config_.reactionHoldSeconds)
                             : 0.0f;
    }

    Config config_;
    BehaviorState state_ = BehaviorState::Idle;
    float timeInState_ = 0.0f;
    float reactionTimer_ = 0.0f;
    ExternalStateOverride externalOverride_{};
};

}  // namespace ppet::model
