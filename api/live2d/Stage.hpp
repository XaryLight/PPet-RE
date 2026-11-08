// PPet - C++ 移植版
// Live2D 运行时分层（参考 FULASKA/-live2d- 的 SPEC：L0–L5 仲裁 / 姿态总线 /
// 三通道视线 / 待机微摆与随机成套动作 / 随机小表情 / 多边形热区手势 / 口型）。
//
// 【为什么单独一层】
//   原 ppet 的 Live2D 是把视线/眨眼/呼吸/动作直接往 Cubism 参数上写，多层相互
//   覆盖，动作结束后参数「卡在最后一帧」。FULASKA 的核心经验是：参数写入必须
//   分层 + 每帧从默认姿态重建 + 加性层物理前后各 flush 一次，才能避免互相踩踏。
//   本项目已有 MotionDirector 负责「播哪个动作 + 优先级 + fade」，本文件负责
//   「参数怎么分层写入」，两者解耦协作。
//
// 【纯逻辑约束】
//   本文件不碰 Cubism / GL / Qt / 文件 IO。它只维护「参数名 -> 最终值」的映射，
//   由 Live2DModel 负责把映射刷进 Cubism 参数。因此可脱离 SDK 做单元测试。
//
// 【分层约定】（Layer，数值越大越晚写、越优先）
//   L0 默认姿态（由 Cubism 的 ResetParametersToDefault 提供，本层不写）
//   L1 视线追踪（眼/头/身体）
//   L2 姿态总线（抚摸/捏脸/挠痒的加性偏移，物理前后各 flush）
//   L3 待机微摆 + 待机随机成套动作（IdleDirector 产出，叠加在基线上）
//   L4 表情/小表情（IdleFaces 加性）
//   L5 强制互动（FORCE 打断，必定覆盖）
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace ppet::live2d {

/// 参数层。
enum class ParamLayer {
    Gaze = 1,       ///< L1 视线追踪
    Pose = 2,       ///< L2 姿态总线（加性）
    Idle = 3,       ///< L3 待机微摆 + 随机成套动作
    Face = 4,       ///< L4 小表情（加性）
    Force = 5,      ///< L5 强制互动
};

/// 写入模式。
enum class WriteMode {
    Override,  ///< 覆盖（同层后者覆盖前者；高层覆盖低层）
    Add,       ///< 加性（累加，常用于姿态总线与小表情）
};

/// 分层参数栈：每帧从默认姿态重建，按层序写入，高层优先。
///
/// 用法：
///   stack.Reset(defaultValues);          // 每帧开始：清空，回到默认姿态
///   stack.Write(Gaze, "ParamAngleX", x); // 视线层写头部角度
///   stack.Write(Pose, "ParamAngleX", dx, Add); // 姿态层加性偏移（抚摸）
///   stack.Write(Force, "ParamAngleX", fx);     // 强制层覆盖
///   auto& values = stack.Commit();       // 得到最终「参数名 -> 值」
class ParameterStack {
public:
    /// 每帧开始：以默认姿态为基线，清空所有层贡献。
    void Reset(const std::map<std::string, float>& defaults) {
        defaults_ = defaults;
        layers_.clear();  // 各层贡献复位（退出复位：层不再写就自动回落）
    }

    /// 写入一层对某个参数的贡献。
    void Write(ParamLayer layer, const std::string& id, float value, WriteMode mode = WriteMode::Override) {
        layers_[static_cast<int>(layer)][id].push_back({value, mode});
    }

    /// 提交：按 L1..L5 顺序合成最终值，默认层（L0）提供基线。
    /// @return 「参数名 -> 值」映射，调用方据此刷 Cubism 参数。
    const std::map<std::string, float>& Commit() {
        out_ = defaults_;
        // 按层顺序（1..5）写入：低层先写，高层覆盖/累加。
        for (int l = 1; l <= 5; ++l) {
            auto it = layers_.find(l);
            if (it == layers_.end()) continue;
            for (const auto& kv : it->second) {
                const std::string& id = kv.first;
                for (const Contribution& c : kv.second) {
                    auto outIt = out_.find(id);
                    const float base = outIt == out_.end() ? 0.0f : outIt->second;
                    if (c.mode == WriteMode::Add) {
                        out_[id] = base + c.value;
                    } else {
                        out_[id] = c.value;  // 覆盖：同层后者覆盖前者
                    }
                }
            }
        }
        return out_;
    }

    const std::map<std::string, float>& Values() const { return out_; }

private:
    struct Contribution {
        float value;
        WriteMode mode;
    };
    std::map<std::string, float> defaults_;
    std::map<std::string, float> out_;
    std::map<int, std::map<std::string, std::vector<Contribution>>> layers_;
};

/// 姿态总线：所有「加性偏移」（抚摸/捏脸/挠痒/拖拽）只累加增量，
/// 由 Live2DModel 在物理计算前后各 Flush 一次，避免互相覆盖。
///
/// 设计要点（对齐 FULASKA 的 pose-bus.js）：
///   · 视线、待机、动作各写各的；只有「用户手势」走姿态总线；
///   · 一次手势可能在一帧内多次 Add（多个区域同时触发），汇总后再应用；
///   · Flush 返回本帧累计增量并清空，保证「下一帧从零起步」。
class PoseBus {
public:
    /// 累加一个加性增量。
    void Add(const std::string& id, float delta) { acc_[id] += delta; }

    /// 取出本帧累计增量并清空。
    std::map<std::string, float> Flush() {
        std::map<std::string, float> out = std::move(acc_);
        acc_.clear();
        return out;
    }

    /// 是否有待应用的手势增量。
    bool HasPending() const { return !acc_.empty(); }

    void Clear() { acc_.clear(); }

private:
    std::map<std::string, float> acc_;
};

/// 三通道视线：眼 / 头 / 身体各自指数阻尼追向目标，方向非对称增益
/// （回到中心更快、偏离中心更慢，符合真实视线习惯）。
///
/// 输入是「归一化鼠标方向」(-1..1)，输出是各通道阻尼后的角度/偏移，
/// 由 Live2DModel 乘以各自最大偏转角后刷入 ParamAngleX / ParamAngleY /
/// ParamEyeBallX / ParamBodyAngleX 等。
class GazeChannels {
public:
    struct Channels {
        float eyeX = 0.0f;   ///< 眼球左右
        float eyeY = 0.0f;   ///< 眼球上下
        float headX = 0.0f;  ///< 头部左右
        float headY = 0.0f;  ///< 头部上下
        float bodyX = 0.0f;  ///< 身体左右
    };

    /// 三通道各自的响应速度（越大追得越快），单位 1/秒。
    void SetSpeeds(float eye, float head, float body) {
        eyeSpeed_ = eye;
        headSpeed_ = head;
        bodySpeed_ = body;
    }

    /// 设置方向非对称增益：回到中心速度（center）与偏离速度（away）。
    void SetAsymmetry(float towardCenter, float towardAway) {
        centerGain_ = towardCenter;
        awayGain_ = towardAway;
    }

    /// 每帧推进。targetX/Y 为归一化鼠标方向 (-1..1)。无鼠标时传 0 回中。
    Channels Tick(float dt, float targetX, float targetY) {
        Channels out;
        const float aX = DampTo(eyeX_, targetX, dt, eyeSpeed_);
        const float aY = DampTo(eyeY_, targetY, dt, eyeSpeed_);
        const float hX = DampTo(headX_, targetX, dt, headSpeed_);
        const float hY = DampTo(headY_, targetY, dt, headSpeed_);
        const float bX = DampTo(bodyX_, targetX * 0.5f, dt, bodySpeed_);
        eyeX_ = aX; eyeY_ = aY; headX_ = hX; headY_ = hY; bodyX_ = bX;
        out.eyeX = aX; out.eyeY = aY; out.headX = hX; out.headY = hY; out.bodyX = bX;
        return out;
    }

    const Channels& Current() const {
        static Channels c;
        c.eyeX = eyeX_; c.eyeY = eyeY_; c.headX = headX_; c.headY = headY_; c.bodyX = bodyX_;
        return c;
    }

    void Reset() { eyeX_ = eyeY_ = headX_ = headY_ = bodyX_ = 0.0f; }

private:
    float DampTo(float current, float target, float dt, float speed) const {
        // 方向相关增益：朝中心（current 与 target 同号且 target 更接近 0）用 centerGain_，
        // 朝远离中心用 awayGain_。简化：用 target 的符号判断「是否在回中」。
        const bool recentering = (current * target <= 0.0f) || (std::fabs(target) < std::fabs(current));
        const float gain = recentering ? centerGain_ : awayGain_;
        const float alpha = 1.0f - std::exp(-gain * speed * dt);
        return current + (target - current) * alpha;
    }

    float eyeSpeed_ = 20.0f;   // 眼球快（约 20/s）
    float headSpeed_ = 7.0f;   // 头较慢（约 7/s）
    float bodySpeed_ = 3.5f;   // 身体最慢（约 3.5/s）
    float centerGain_ = 1.6f;  // 回中更快
    float awayGain_ = 1.0f;    // 偏离更慢

    float eyeX_ = 0.0f, eyeY_ = 0.0f, headX_ = 0.0f, headY_ = 0.0f, bodyX_ = 0.0f;
};

/// 待机导演：常驻微摆 + 每 8–18 秒随机一套「成套动作」+ 钟形包络 + 让位。
///
/// 「成套动作」指一组可以同时叠加的待机偏移（例如：身体微转 + 头微微歪 +
/// 呼吸加深），共享同一个钟形（bell）包络，让动作有「起承转合」而不是匀速。
/// 动作进行中不抢其它层；动作结束自动回落，由 Live2DModel 的冷却逻辑重排。
class IdleDirector {
public:
    struct Sway {
        float headYaw = 0.0f;    ///< 头部左右微摆
        float headPitch = 0.0f;  ///< 头部上下微摆
        float breath = 0.0f;     ///< 呼吸起伏（叠加在 ParamBreath/ParamAngleY 上）
    };

    void SetRandomRange(float base, float jitter) {
        base_ = base;
        jitter_ = jitter;
    }
    void SetSwayAmplitude(float yaw, float pitch, float breath) {
        swayYawAmp_ = yaw;
        swayPitchAmp_ = pitch;
        breathAmp_ = breath;
    }

    /// 每帧推进。
    /// @param dt         时间步长
    /// @param talkMask   0..1，说话/互动时压低待机微摆（避免「边说话边大幅晃」）
    /// @return 当前微摆偏移
    Sway Tick(float dt, float talkMask = 0.0f) {
        t_ += dt;
        talkMask = std::clamp(talkMask, 0.0f, 1.0f);
        Sway sway;
        const float mask = 1.0f - 0.7f * talkMask;
        sway.headYaw = std::sin(t_ * 0.7f) * swayYawAmp_ * mask;
        sway.headPitch = std::sin(t_ * 1.6f) * swayPitchAmp_ * mask;
        sway.breath = std::sin(t_ * 1.1f) * breathAmp_ * mask;
        return sway;
    }

    /// 是否正在播放一套随机待机动作（钟形包络进行中）。
    bool IsActionActive() const { return actionTime_ < actionDuration_; }

    /// 当前随机动作的钟形包络权重 [0,1]（起承转合）。
    float ActionEnvelope() const {
        if (actionDuration_ <= 0.0f) return 0.0f;
        const float x = std::clamp(actionTime_ / actionDuration_, 0.0f, 1.0f);  // 0..1
        return std::sin(x * 3.14159265f) * actionWeight_;
    }

    /// 启动一套随机待机动作（由 Live2DModel 的待机冷却触发）。
    void StartAction(float duration, float weight) {
        actionTime_ = 0.0f;
        actionDuration_ = duration;
        actionWeight_ = weight;
    }

    void AdvanceAction(float dt) {
        if (actionTime_ < actionDuration_) actionTime_ += dt;
    }

private:
    float t_ = 0.0f;
    float swayYawAmp_ = 0.0262f;
    float swayPitchAmp_ = 0.0131f;
    float breathAmp_ = 0.05f;
    float base_ = 8.0f;
    float jitter_ = 10.0f;

    float actionTime_ = 1.0f;
    float actionDuration_ = 1.0f;
    float actionWeight_ = 0.0f;
};

/// 一个待机小表情预设。
///
/// Live2D 的「表情」有两种载体：
///   1. 表情动作组（model3.json 的 Motions 里名字含 "Exp" 的组）——本项目用
///      MotionDirector 播放；
///   2. **表情参数**（ParamBrowLY / ParamCheek / ParamEyeLSmile …）——
///      模型直接用参数驱动眉毛、脸颊、笑眼，最常见也最轻量。
///
/// 很多模型（包括本项目实测的国内模型包）**没有任何表情动作组**，
/// 只有第 2 种载体。因此随机表情必须走参数通道，否则「待机表情随机化」
/// 对这类模型完全无效。
struct FacePreset {
    std::string id;    ///< 预设 id（如 "happy"）
    std::string name;  ///< 显示名（如 "开心"）
    /// 参数名 -> 目标值。值为 0 表示「回到中性」。
    std::vector<std::pair<std::string, float>> params;
};

/// 待机小表情：随机挑一个预设，淡入 -> 停留 -> 淡出回中性。
///
/// 与「手动切换表情」的区别（对齐 VRM-VPet 的随机表情）：
///   · 手动切换会一直保持到下一次手动操作；
///   · 随机表情是「换一个表情 → 停留 2~5 秒 → 自动回到中性」的循环，
///     且两次随机之间的间隔是随机的（默认 8~20 秒）。
///
/// 输出「参数名 -> 权重」，由 Live2DModel **叠加**刷入模型参数
/// （叠加而不是覆盖，保证模型自己的待机动作与表情可以共存）。
class IdleFaces {
public:
    struct Face {
        std::string name;   ///< 参数名
        float weight = 0.0f; ///< 加性权重
    };

    /// 设置可用的小表情预设。
    void SetPresets(std::vector<FacePreset> presets, std::uint32_t seed = 13579u) {
        presets_ = std::move(presets);
        rng_.seed(seed);
        cooldown_ = RandomCooldown();
    }

    /// 设置候选小表情名（单参数脉冲模式）。
    ///
    /// 这是早期接口：只给某个参数一个瞬时加性权重。
    /// 保留是为了兼容「模型只有一个可调表情参数」的最简场景。
    void SetCandidates(const std::vector<std::string>& candidates, std::uint32_t seed = 13579u) {
        candidates_ = candidates;
        rng_.seed(seed);
    }

    /// 重置运行态（重新加载模型 / 控制台 re model 时调用）。
    /// 只清「正在播放的表情」，保留预设、随机间隔与开关配置。
    /// 不做这一步，上一个模型/上一次状态残留的手动表情会渗进新画面的第一帧。
    void Reset() {
        presetIndex_ = -1;
        phase_ = Phase::Idle;
        phaseTime_ = 0.0f;
        manual_ = false;
        pulseName_.clear();
        pulseWeight_ = 0.0f;
        pulseTime_ = -1.0f;
        winkTime_ = -1.0f;
        cooldown_ = RandomCooldown();
    }

    /// 是否已配置可用预设。
    bool Empty() const { return presets_.empty() && candidates_.empty(); }

    /// 当前可用预设（诊断用）。
    const std::vector<FacePreset>& Presets() const { return presets_; }

    /// 开启/关闭随机小表情。
    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        if (enabled) {
            cooldown_ = RandomCooldown();
        } else if (!manual_) {
            // 关闭自动随机时立即回中性，避免表情卡在非中性状态；
            // 但不要打断「手动触发、还在淡入/停留」的那一次表情。
            presetIndex_ = -1;
            phase_ = Phase::Idle;
            phaseTime_ = 0.0f;
        }
    }
    bool Enabled() const { return enabled_; }

    /// 所有预设 id（诊断 / 控制台 expression list 用）。
    std::vector<std::string> PresetIds() const {
        std::vector<std::string> ids;
        ids.reserve(presets_.size());
        for (const FacePreset& p : presets_) ids.push_back(p.id);
        return ids;
    }

    /// 设置两次随机之间的间隔区间（秒）。
    void SetInterval(float min, float max) {
        min_ = std::max(1.0f, min);
        max_ = std::max(min_, max);
    }

    /// 每帧推进，返回当前应叠加到模型参数上的「参数 -> 权重」。
    std::vector<Face> Tick(float dt) {
        if (!(dt > 0.0f)) return CurrentFaces();
        // 关闭「自动随机」时，仍要让「手动触发」的这一次表情完整播完
        // （淡入 → 停留 → 淡出 → 回中性）。否则控制台 expression <id>、
        // 托盘「立即触发随机表情」在开关关闭时全部静默失效。
        if (!enabled_ && !manual_) return CurrentFaces();

        // 单参数脉冲模式：权重随相位衰减，到期后清空。
        if (pulseTime_ >= 0.0f) {
            pulseTime_ += dt;
            if (pulseTime_ >= kPulseDuration) pulseTime_ = -1.0f;
        }
        // wink 覆盖层：单眼眨同样需要推进相位，否则触发后权重恒为 0。
        if (winkTime_ >= 0.0f) {
            winkTime_ += dt;
            if (winkTime_ >= kWinkDuration) winkTime_ = -1.0f;
        }

        // 状态机：Idle（等待） -> FadeIn（淡入） -> Hold（停留） -> FadeOut（淡出）
        switch (phase_) {
            case Phase::Idle: {
                if (!enabled_) {
                    // 手动触发表情已播完，且自动随机关闭：停在这里等下一次触发。
                    manual_ = false;
                    break;
                }
                cooldown_ -= dt;
                if (cooldown_ <= 0.0f) StartPreset();
                break;
            }
            case Phase::FadeIn: {
                phaseTime_ += dt;
                if (phaseTime_ >= fadeTime_) {
                    phase_ = Phase::Hold;
                    phaseTime_ = 0.0f;
                }
                break;
            }
            case Phase::Hold: {
                phaseTime_ += dt;
                if (phaseTime_ >= holdTime_) {
                    phase_ = Phase::FadeOut;
                    phaseTime_ = 0.0f;
                }
                break;
            }
            case Phase::FadeOut: {
                phaseTime_ += dt;
                if (phaseTime_ >= fadeTime_) {
                    phase_ = Phase::Idle;
                    phaseTime_ = 0.0f;
                    presetIndex_ = -1;
                    cooldown_ = RandomCooldown();
                    manual_ = false;  // 这一轮（随机或手动）表情结束
                }
                break;
            }
        }
        return CurrentFaces();
    }

    /// 立刻随机抽一个小表情并开始播放（返回是否抽到了）。
    ///
    /// 这是「手动触发」入口：即使 SetEnabled(false) 关闭了自动随机，
    /// 也要让这一次表情完整播放——由 manual_ 标记驱动 Tick 继续推进状态机。
    bool Trigger() {
        manual_ = StartPreset();
        return manual_;
    }

    /// 按 id 触发指定预设（控制台 expression <id> / 手动切换表情用）。
    /// 找不到该 id 时返回 false，且不改动当前状态。
    bool TriggerId(const std::string& id) {
        for (std::size_t i = 0; i < presets_.size(); ++i) {
            if (presets_[i].id == id) {
                manual_ = StartPreset(static_cast<int>(i));
                return manual_;
            }
        }
        return false;
    }

    /// 触发一次 wink（单眼眨）。
    void TriggerWink() { if (winkTime_ < 0.0f) winkTime_ = 0.0f; }

    /// 当前小表情 id（空串 = 当前是中性）。
    std::string CurrentId() const {
        if (presetIndex_ < 0 || static_cast<std::size_t>(presetIndex_) >= presets_.size()) {
            return std::string();
        }
        return presets_[static_cast<std::size_t>(presetIndex_)].id;
    }

    /// 距离下一次随机小表情还有多久（秒）。
    float TimeToNext() const {
        if (phase_ != Phase::Idle) return 0.0f;
        return std::max(0.0f, cooldown_);
    }

private:
    enum class Phase { Idle, FadeIn, Hold, FadeOut };

    static constexpr float kWinkDuration = 0.18f;
    static constexpr float kPulseDuration = 0.35f;
    static constexpr char kWinkLeft[] = "ParamWinkL";
    static constexpr char kWinkRight[] = "ParamWinkR";

    /// 当前生效的小表情权重（按参数名聚合）。
    std::vector<Face> CurrentFaces() const {
        std::vector<Face> out;
        // 预设权重：淡入淡出包络 × 预设目标值。
        if (presetIndex_ >= 0 && static_cast<std::size_t>(presetIndex_) < presets_.size()) {
            const float envelope = Envelope();
            if (envelope > 0.0f) {
                const FacePreset& preset = presets_[static_cast<std::size_t>(presetIndex_)];
                for (const auto& kv : preset.params) {
                    out.push_back({kv.first, kv.second * envelope});
                }
            }
        }
        // 单参数脉冲模式：一个瞬时小表情（权重随相位衰减）。
        if (pulseTime_ >= 0.0f) {
            const float ph = std::min(1.0f, pulseTime_ / kPulseDuration);
            const float v = std::sin(ph * 3.14159265f);
            if (!pulseName_.empty()) out.push_back({pulseName_, pulseWeight_ * v});
        }
        // wink 覆盖层：偶发单眼眨，权重随相位衰减。
        if (winkTime_ >= 0.0f) {
            const float ph = std::min(1.0f, winkTime_ / kWinkDuration);
            const float v = std::sin(ph * 3.14159265f);
            out.push_back({kWinkLeft, v});
            out.push_back({kWinkRight, v});
        }
        return out;
    }

    /// 当前包络权重 [0,1]。
    float Envelope() const {
        switch (phase_) {
            case Phase::FadeIn:
                return fadeTime_ > 0.0f ? std::min(1.0f, phaseTime_ / fadeTime_) : 1.0f;
            case Phase::Hold:
                return 1.0f;
            case Phase::FadeOut:
                return fadeTime_ > 0.0f
                           ? std::max(0.0f, 1.0f - phaseTime_ / fadeTime_)
                           : 0.0f;
            case Phase::Idle:
            default:
                return 0.0f;
        }
    }

    /// 挑一个预设并进入淡入阶段。index < 0 表示随机，否则用指定下标。
    bool StartPreset(int index = -1) {
        if (!presets_.empty()) {
            if (index < 0 || index >= static_cast<int>(presets_.size())) {
                std::uniform_int_distribution<std::size_t> dist(0, presets_.size() - 1);
                index = static_cast<int>(dist(rng_));
            }
            presetIndex_ = index;
            phase_ = Phase::FadeIn;
            phaseTime_ = 0.0f;
            std::uniform_real_distribution<float> fade(0.25f, 0.5f);
            std::uniform_real_distribution<float> hold(2.0f, 5.0f);
            fadeTime_ = fade(rng_);
            holdTime_ = hold(rng_);
            return true;
        }
        // 单参数脉冲模式：只给一个候选参数一个瞬时权重。
        if (!candidates_.empty()) {
            std::uniform_int_distribution<std::size_t> dist(0, candidates_.size() - 1);
            pulseName_ = candidates_[dist(rng_)];
            std::uniform_real_distribution<float> w(0.2f, 0.5f);
            pulseWeight_ = w(rng_);
            pulseTime_ = 0.0f;
            // 脉冲模式不走预设状态机，需要自己重置冷却，否则会每帧都触发。
            cooldown_ = RandomCooldown();
            return true;
        }
        return false;
    }

    /// 两次随机之间的等待时间。
    float RandomCooldown() {
        std::uniform_real_distribution<float> dist(min_, max_);
        return dist(rng_);
    }

    std::vector<FacePreset> presets_;
    std::vector<std::string> candidates_;
    bool enabled_ = true;
    float min_ = 8.0f;   ///< 随机间隔下限（秒）
    float max_ = 20.0f;  ///< 随机间隔上限（秒）
    float cooldown_ = 5.0f;

    int presetIndex_ = -1;
    Phase phase_ = Phase::Idle;
    float phaseTime_ = 0.0f;
    float fadeTime_ = 0.4f;
    float holdTime_ = 3.0f;

    std::string pulseName_;
    float pulseWeight_ = 0.0f;
    float pulseTime_ = -1.0f;

    float winkTime_ = -1.0f;
    /// 当前这一轮表情是否由手动触发（决定 enabled_=false 时是否继续推进）。
    bool manual_ = false;
    std::mt19937 rng_{13579u};
};

/// 多边形热区集合：区域带 z 优先级（z 越大越优先命中），支持 tap / pet / drag
/// 三种手势，以及「区域长期开关态」（例如一直摸头会让模型进入「被摸爽了」状态）。
///
/// 坐标与 GLFW 光标一致：nx, ny ∈ [0,1]，原点左上、y 向下。
class RegionSet {
public:
    struct Region {
        std::string name;
        std::vector<std::pair<float, float>> polygon;  ///< 归一化多边形顶点
        int z = 0;                                     ///< 优先级，越大越先命中
        std::string gesture;                            ///< "tap" / "pet" / "drag"
    };

    void Add(const Region& region) { regions_.push_back(region); }

    /// 命中测试：返回最优先（z 最大）且包含点的区域名；无命中返回空串。
    std::string Hit(float nx, float ny) const {
        int bestZ = -1;
        std::string best;
        for (const Region& r : regions_) {
            if (r.z > bestZ && PointInPolygon(nx, ny, r.polygon)) {
                bestZ = r.z;
                best = r.name;
            }
        }
        return best;
    }

    /// 区域手势分类（直接返回区域自带的手势类型）。
    std::string GestureOf(const std::string& name) const {
        for (const Region& r : regions_) if (r.name == name) return r.gesture;
        return std::string();
    }

    /// 长期开关态：记录每个区域被持续触发的时间，超过阈值进入「长期态」。
    /// 只有当前 activeRegion 累加时间，其它区域清零（松手后长期态自然解除）。
    void UpdateHoldTime(float dt, const std::string& activeRegion) {
        for (auto& kv : holdTime_) {
            if (kv.first != activeRegion) kv.second = 0.0f;
        }
        if (!activeRegion.empty()) holdTime_[activeRegion] += dt;
    }

    /// 某区域是否处于长期态（被持续触发 >= threshold 秒）。
    bool IsLongHeld(const std::string& name, float threshold = 2.0f) const {
        auto it = holdTime_.find(name);
        return it != holdTime_.end() && it->second >= threshold;
    }

    std::size_t Size() const { return regions_.size(); }

private:
    static bool PointInPolygon(float x, float y, const std::vector<std::pair<float, float>>& poly) {
        if (poly.size() < 3) return false;
        bool inside = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const float xi = poly[i].first, yi = poly[i].second;
            const float xj = poly[j].first, yj = poly[j].second;
            const bool intersect = ((yi > y) != (yj > y)) &&
                (x < (xj - xi) * (y - yi) / (yj - yi) + xi);
            if (intersect) inside = !inside;
        }
        return inside;
    }

    std::vector<Region> regions_;
    std::map<std::string, float> holdTime_;
};

/// 口型导演：待机呼吸微动 + 说话音节（由 LipSync 驱动）+ 情绪嘴形。
/// 输出开口度与嘴形，刷入 ParamMouthOpenY / ParamMouthForm。
/// 本类只做「平滑与限幅」，说话能量来自外部 LipSync，避免重复实现音节逻辑。
class MouthDirector {
public:
    void SetRange(float minOpen, float maxOpen) { minOpen_ = minOpen; maxOpen_ = maxOpen; }

    /// 每帧推进。
    /// @param dt          时间步长
    /// @param talkOpen    说话开口度 [0,1]（来自 LipSync，0=闭嘴）
    /// @return open ∈ [minOpen,maxOpen]，form ∈ [-1,1]
    std::pair<float, float> Tick(float dt, float talkOpen) {
        breathPhase_ += dt * 1.1f;
        const float breath = (std::sin(breathPhase_ * 2.0f * 3.14159265f) * 0.5f + 0.5f) * 0.03f;
        const float target = std::min(1.0f, breath + talkOpen);
        open_ += (target - open_) * (1.0f - std::exp(-18.0f * dt));  // 快速追向目标
        return {minOpen_ + (maxOpen_ - minOpen_) * open_, form_};
    }

    void SetForm(float form) { form_ = std::clamp(form, -1.0f, 1.0f); }
    float Open() const { return minOpen_ + (maxOpen_ - minOpen_) * open_; }
    float Form() const { return form_; }

private:
    float minOpen_ = 0.0f;
    float maxOpen_ = 1.0f;
    float open_ = 0.0f;
    float form_ = 0.0f;
    float breathPhase_ = 0.0f;
};

}  // namespace ppet::live2d
