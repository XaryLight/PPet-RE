// PPet - C++ 移植版
// 程序化待机动画：让 PMX / VRM 模型在待机时「活起来」。
//
// 【为什么需要它】
//   VRM-VPet（参考实现）里的桌宠在待机时并不是静止的：模型会呼吸、
//   重心左右转移、头部有延迟的微摆、手臂自然摆动，偶尔还会来一段小动作。
//   本项目没有 Unity 的 Animator 与动画资源，只能用**程序化**的方式生成这些动作：
//   由若干条不同频率的正弦曲线叠加而成，再叠加随机的小动作（gesture）。
//
//   早期版本的待机只有「躯干 1.5° 摆动」，几乎看不出来，本模块把幅度与
//   通道数都补齐到接近参考实现的观感。
//
// 【设计要点】
//   1. 纯头文件、零外部依赖（不碰 GL / Qt / 文件 IO），因此可以直接单测；
//   2. 输出是**相对绑定姿态的偏移量**（弧度 / 米），由各后端自行套用到骨骼上；
//   3. 每一层都用**互质或近似互质**的频率，避免出现明显的循环节拍；
//   4. 小动作按 8~18 秒随机间隔触发（与 FULASKA 的待机节奏一致），
//      用钟形包络淡入淡出，并在动作期间压低基础微摆，避免「叠加抽搐」。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace ppet::model {

/// 一帧的程序化待机姿态。
///
/// 所有角度单位是**弧度**，位移单位是**模型单位**（PMX/VRM 的世界尺度）。
struct IdlePose {
    // --- 躯干 ---
    float spinePitch = 0.0f;   ///< 躯干前倾/后仰
    float spineYaw = 0.0f;     ///< 躯干左右转
    float spineRoll = 0.0f;    ///< 躯干侧倾
    float spineLift = 0.0f;    ///< 躯干上下位移（呼吸时轻微起伏）
    float chestScale = 0.0f;   ///< 胸腔缩放（呼吸，正值=扩张）

    // --- 头部 ---
    float headPitch = 0.0f;
    float headYaw = 0.0f;
    float headRoll = 0.0f;

    // --- 手臂 ---
    float leftArmPitch = 0.0f;   ///< 左大臂前后摆
    float leftArmRoll = 0.0f;    ///< 左大臂侧摆
    float rightArmPitch = 0.0f;
    float rightArmRoll = 0.0f;
    float leftForearmPitch = 0.0f;   ///< 左小臂弯曲
    float rightForearmPitch = 0.0f;  ///< 右小臂弯曲

    // --- 腿 / 重心 ---
    float leftLegLift = 0.0f;   ///< 左腿抬起高度（重心转移时）
    float rightLegLift = 0.0f;
    float weightShift = 0.0f;   ///< 重心左右偏移（-1=偏左，+1=偏右）

    // --- 其它 ---
    float eyeSquint = 0.0f;     ///< 眼睛微眯（0=正常，1=完全眯起）
};

/// 程序化待机动画器。
///
/// 用法：每帧调用 Tick(dt)，拿到 IdlePose 后由后端套到骨骼/形变上。
/// 所有随机性都由内部种子驱动（可复现）。
class IdleAnimator {
public:
    /// 可调参数。
    struct Config {
        /// 呼吸频率（次/秒）。真人静息约 0.2~0.3 Hz，这里取 0.22。
        float breathSpeed = 0.24f;
        /// 呼吸幅度倍率（1 = 默认幅度）。
        float breathAmount = 1.0f;
        /// 重心摆动频率（次/秒）。
        float swaySpeed = 0.085f;
        /// 重心摆动幅度倍率。
        float swayAmount = 1.0f;
        /// 头部微摆频率（次/秒）。
        float headSpeed = 0.13f;
        /// 头部微摆幅度倍率。
        float headAmount = 1.0f;
        /// 手臂摆动频率（次/秒）。
        float armSpeed = 0.11f;
        /// 手臂摆动幅度倍率。
        float armAmount = 1.0f;
        /// 小动作的最小间隔（秒）。
        float gestureMinInterval = 8.0f;
        /// 小动作的最大间隔（秒）。
        float gestureMaxInterval = 18.0f;
        /// 小动作时长（秒）。
        float gestureDuration = 2.4f;
        /// 「伸懒腰」抬臂幅度倍率。旧版 0.55 rad（≈31°）会让模型看起来
        /// 老在举手，这里默认压到 0.35 倍（≈11°），更像自然舒展。
        float stretchArmAmount = 0.35f;
        /// 整体幅度倍率（0 = 完全静止）。
        float masterAmount = 1.0f;
    };

    /// 小动作类型。
    enum class Gesture {
        None,        ///< 无
        WeightShift, ///< 换重心（左右晃两下）
        Stretch,     ///< 伸懒腰（抬胸 + 展臂）
        LookAround,  ///< 东张西望（头部大幅转动）
        Breathe,     ///< 深吸一口气（胸腔大幅起伏）
    };

    explicit IdleAnimator(std::uint32_t seed = 20240501u) : rng_(seed) {
        Reset();
    }

    /// 设置参数。
    void SetConfig(const Config& config) { config_ = config; }

    /// 当前参数。
    const Config& GetConfig() const { return config_; }

    /// 随机种子（重置为同一个种子可复现同样的待机节奏）。
    void Seed(std::uint32_t seed) {
        rng_.seed(seed);
        Reset();
    }

    /// 复位到初始状态（时间归零、姿态清零）。
    void Reset() {
        time_ = 0.0;
        gestureTimer_ = NextInterval();
        gesture_ = Gesture::None;
        gestureTime_ = 0.0f;
        gestureDuration_ = config_.gestureDuration;
        pose_ = IdlePose();
    }

    /// 每帧推进并返回当前姿态。
    IdlePose Tick(float dt) {
        if (!(dt > 0.0f)) return pose_;
        // 切后台回来时 dt 可能非常大，钳制以免姿态瞬移。
        dt = std::min(dt, 0.1f);
        // 【长期运行】时间用 double 累加，且每条相位都对 2π 取模。
        // 若用 float 累加，跑上几天后 time_ 会到 1e5~1e6 量级，float 只有
        // 7 位有效数字，sin 的相位分辨率退化到 0.1 弧度以上，待机动作会
        // 变成一格一格跳（甚至卡住）。取模后相位永远保持在 [0, 2π)。
        time_ += static_cast<double>(dt);

        // --- 小动作调度 ---
        gestureTimer_ -= dt;
        if (gesture_ == Gesture::None && gestureTimer_ <= 0.0f) {
            StartGesture();
        }
        if (gesture_ != Gesture::None) {
            gestureTime_ += dt;
            if (gestureTime_ >= gestureDuration_) {
                gesture_ = Gesture::None;
                gestureTimer_ = NextInterval();
            }
        }

        // --- 基础待机 ---
        // 【长期运行】相位用 double 计算：time_ 是 double，跑到 1e9 秒（约 31 年）
        // 时相位仍有 1e-6 弧度级分辨率；若用 float 累加，几天后就会退化成跳帧。
        // 这里刻意**不**对相位取模：sin(headPhase * 1.7) 这类派生项依赖相位的绝对值，
        // 单独把基础相位 fmod 回 [0,2π) 会让派生项在每个周期边界突变。
        const float master = std::max(0.0f, config_.masterAmount);
        constexpr double kTwoPi = 6.28318530717958647692;
        const double breathPhase = time_ * static_cast<double>(config_.breathSpeed) * kTwoPi;
        const double swayPhase = time_ * static_cast<double>(config_.swaySpeed) * kTwoPi;
        const double headPhase = time_ * static_cast<double>(config_.headSpeed) * kTwoPi;
        const double armPhase = time_ * static_cast<double>(config_.armSpeed) * kTwoPi;

        // 呼吸：正弦驱动，但用 sin 的绝对值形状更接近「吸-呼-停」的真实节奏。
        const float breath = static_cast<float>(std::sin(breathPhase));
        const float breathAmp = config_.breathAmount * master;

        // 重心：低频左右摆（用两个不同频率叠加，避免机械感）。
        const float sway = static_cast<float>(std::sin(swayPhase) * 0.7 +
                                             std::sin(swayPhase * 1.618) * 0.3);
        const float swayAmp = config_.swayAmount * master;

        // 头部：与身体反相（真人走路时头会略微反向稳定视线）。
        const float head = static_cast<float>(std::sin(headPhase) * 0.6 +
                                             std::sin(headPhase * 2.31) * 0.4);
        const float headAmp = config_.headAmount * master;

        // 手臂：低频小幅摆动，左右反相（走路时的自然摆臂）。
        const float arm = static_cast<float>(std::sin(armPhase));
        const float armAmp = config_.armAmount * master;

        IdlePose pose;
        pose.spinePitch = breath * 0.012f * breathAmp;
        pose.spineYaw = sway * 0.055f * swayAmp;
        pose.spineRoll = sway * 0.030f * swayAmp;
        pose.spineLift = breath * 0.012f * breathAmp;
        pose.chestScale = breath * 0.016f * breathAmp;

        pose.headYaw = head * 0.070f * headAmp - sway * 0.030f * swayAmp;
        pose.headPitch = static_cast<float>(std::sin(headPhase * 1.7)) * 0.045f * headAmp;
        pose.headRoll = static_cast<float>(std::sin(headPhase * 0.8)) * 0.035f * headAmp;

        pose.leftArmPitch = -arm * 0.075f * armAmp;
        pose.rightArmPitch = arm * 0.075f * armAmp;
        pose.leftArmRoll = sway * 0.045f * swayAmp;
        pose.rightArmRoll = -sway * 0.045f * swayAmp;
        pose.leftForearmPitch =
            static_cast<float>(0.5 + 0.5 * std::sin(armPhase * 0.7)) * 0.030f * armAmp;
        pose.rightForearmPitch =
            static_cast<float>(0.5 + 0.5 * std::sin(armPhase * 0.7 + 1.2)) * 0.030f * armAmp;

        // 重心偏移 → 支撑腿与抬腿。
        // 注意：weightShift 也要乘幅度倍率，否则 masterAmount = 0 时重心依旧在平移，
        // 「整体静音」开关会失效（实测由 idle_animator_unit 捕获）。
        pose.weightShift = sway * swayAmp;
        pose.leftLegLift = std::max(0.0f, sway) * 0.020f * swayAmp;
        pose.rightLegLift = std::max(0.0f, -sway) * 0.020f * swayAmp;

        pose.eyeSquint = 0.0f;

        // --- 小动作叠加 ---
        ApplyGesture(pose, master);

        pose_ = pose;
        return pose_;
    }

    /// 当前姿态（不推进）。
    const IdlePose& Current() const { return pose_; }

    /// 当前是否正在播放小动作。
    bool InGesture() const { return gesture_ != Gesture::None; }

    /// 当前小动作类型。
    Gesture CurrentGesture() const { return gesture_; }

    /// 距离下一个小动作还有多久（秒）。
    float TimeToNextGesture() const {
        return gesture_ == Gesture::None ? std::max(0.0f, gestureTimer_) : 0.0f;
    }

    /// 立刻触发一个小动作（用于点击互动等场景）。
    void TriggerGesture(Gesture kind) {
        gesture_ = kind;
        gestureTime_ = 0.0f;
        gestureDuration_ = config_.gestureDuration;
    }

private:
    /// 下一个随机间隔。
    float NextInterval() {
        const float lo = std::max(1.0f, config_.gestureMinInterval);
        const float hi = std::max(lo, config_.gestureMaxInterval);
        std::uniform_real_distribution<float> dist(lo, hi);
        return dist(rng_);
    }

    /// 随机挑一个小动作。
    void StartGesture() {
        static const Gesture kAll[] = {Gesture::WeightShift, Gesture::Stretch,
                                       Gesture::LookAround, Gesture::Breathe};
        std::uniform_int_distribution<int> dist(0, 3);
        gesture_ = kAll[dist(rng_)];
        gestureTime_ = 0.0f;
        // 每个小动作的时长略有差异，避免机械的固定节奏。
        std::uniform_real_distribution<float> dur(config_.gestureDuration * 0.7f,
                                                  config_.gestureDuration * 1.4f);
        gestureDuration_ = std::max(0.4f, dur(rng_));
    }

    /// 钟形包络：0 -> 1 -> 0。
    static float BellEnvelope(float t) {
        if (t <= 0.0f || t >= 1.0f) return 0.0f;
        return std::sin(t * 3.14159265f);
    }

    /// 把小动作叠加到姿态上。
    void ApplyGesture(IdlePose& pose, float master) const {
        if (gesture_ == Gesture::None) return;
        const float t = gestureDuration_ > 0.0f ? gestureTime_ / gestureDuration_ : 1.0f;
        const float env = BellEnvelope(t) * master;
        if (env <= 0.0f) return;

        switch (gesture_) {
            case Gesture::WeightShift:
                pose.spineRoll += std::sin(t * 6.2831853f * 2.0f) * 0.09f * env;
                pose.spineYaw += std::sin(t * 6.2831853f * 2.0f) * 0.10f * env;
                pose.headRoll += std::sin(t * 6.2831853f * 2.0f) * 0.06f * env;
                break;
            case Gesture::Stretch:
                // 旧版固定 0.55 rad（≈31°）的抬臂，模型看起来像一直在举手。
                // 现在由 stretchArmAmount 控制（默认 0.35 倍 ≈11°），是「舒展」而非「举手」。
                pose.chestScale += env * 0.030f;
                pose.spinePitch -= env * 0.045f;
                pose.leftArmPitch -= env * config_.stretchArmAmount * 0.55f;
                pose.rightArmPitch -= env * config_.stretchArmAmount * 0.55f;
                pose.leftForearmPitch -= env * config_.stretchArmAmount * 0.30f;
                pose.rightForearmPitch -= env * config_.stretchArmAmount * 0.30f;
                break;
            case Gesture::LookAround:
                pose.headYaw += std::sin(t * 6.2831853f) * 0.42f * env;
                pose.headPitch += std::sin(t * 6.2831853f * 2.0f) * 0.10f * env;
                break;
            case Gesture::Breathe:
                pose.chestScale += env * 0.045f;
                pose.spinePitch += env * 0.045f;
                pose.spineLift += env * 0.020f;
                break;
            case Gesture::None:
            default:
                break;
        }
    }

    Config config_;
    std::mt19937 rng_;
    /// 累计时间（秒）。必须是 double：见 Tick() 里的长期运行说明。
    double time_ = 0.0;
    float gestureTimer_ = 0.0f;
    float gestureTime_ = 0.0f;
    float gestureDuration_ = 1.0f;
    Gesture gesture_ = Gesture::None;
    IdlePose pose_;
};

}  // namespace ppet::model
