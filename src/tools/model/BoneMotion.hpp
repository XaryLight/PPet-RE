// PPet - C++ 移植版
// 骨骼动作（Bone Motion）：PMX / VRM 共用的程序化肢体动作。
//
// 【对齐 VRM-VPet 的实现】
//   VRM-VPet 的桌宠之所以「有动作」，靠的是**内置骨骼动作**：
//   点头、摇头、挥手等预置动作，各自有明确的骨骼轨道（哪些骨骼、
//   转到什么角度、什么时候转），由一个播放器统一调度：
//     · 播放 / 停止 / 淡入淡出
//     · 一次性动作播完自动回到待机
//     · 待机时随机触发（像 FULASKA 的「每 8~18 秒随机来一段小动作」）
//
// 【与现有程序化待机的关系】
//   IdleAnimator 负责**连续的**待机微动（呼吸、重心、头部晃动），
//   本模块负责**离散的**成套动作（点头 / 摇头 / 挥手）。
//   两者叠加：待机时一直有微动，偶尔来一段明显的动作。
//
// 【纯逻辑约束】
//   本文件不碰 OpenGL / Qt / 文件 IO，只描述「骨骼名 -> 角度」，
//   由 PMX / VRM 后端负责把角度翻译成实际的骨骼旋转。
//   因此可以脱离渲染层做单元测试。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace ppet::model {

/// 骨骼动作里的一条轨道。
///
/// 描述「某个骨骼在动作的某个时间点转到某个角度」。
/// 角度单位是度，与 VRM-VPet 的骨骼动作一致。
///
/// @param bone      骨骼名（如 "head" / "左腕" / "upperArm.L"）
/// @param time      时间点（秒，从动作开始算）
/// @param pitch     X 轴旋转（点头方向）
/// @param yaw       Y 轴旋转（摇头方向）
/// @param roll      Z 轴旋转（歪头方向）
/// @param weight    权重 [0,1]：该关键帧对此骨骼的影响强度
struct BoneKey {
    std::string bone;
    float time = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    float weight = 1.0f;
};

/// 一个骨骼动作（预置动作或用户自定义动作）。
struct BoneMotion {
    std::string id;                        ///< 动作 id（如 "nod"）
    std::string name;                      ///< 显示名（如 "点头"）
    float duration = 1.0f;                 ///< 总时长（秒）
    bool loop = false;                     ///< 是否循环
    std::vector<BoneKey> keys;             ///< 关键帧
};

/// 动作播放器的输出。
///
/// 每帧算出的「骨骼名 -> 角度」增量，由后端叠加到骨骼变换上。
struct BonePose {
    /// 一条骨骼的偏移。
    struct Entry {
        std::string bone;
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
    };
    std::vector<Entry> entries;
};

/// 骨骼动作播放器。
///
/// 职责：
///   · 管理当前播放的动作与播放进度
///   · 淡入淡出（避免动作切换时姿态突变）
///   · 采样关键帧并插值出「骨骼 -> 角度」
///   · 待机时按随机间隔触发预置动作
class BoneMotionPlayer {
public:
    /// 播放一个动作。
    /// @param motion 动作（内部会拷贝一份，避免调用方改动）
    /// @param fadeIn  淡入时长（秒）
    /// @return 动作有效返回 true。
    bool Play(const BoneMotion& motion, float fadeIn = 0.12f);

    /// 停止当前动作（可选择淡出）。
    /// @param fadeOut 淡出时长（秒）；0 表示立即停止。
    void Stop(float fadeOut = 0.2f);

    /// 是否正在播放动作。
    bool IsPlaying() const { return playing_; }

    /// 当前动作的 id（未播放时为空串）。
    const std::string& CurrentId() const { return current_.id; }

    /// 当前动作的播放进度 [0,1]。
    float Progress() const;

    /// 当前动作的有效权重 [0,1]（已含淡入淡出，用 smoothstep 平滑）。
    /// 后端据此让「静态姿态修正」在动作播放期间逐步让位，避免两层姿态打架。
    float Weight() const;

    /// 交叉淡入淡出时长（秒）：切换动作时旧动作淡出、新动作淡入，二者叠加输出。
    /// 默认 0.28 秒。设为 0 则退化为硬切换。
    void SetCrossFade(float seconds) { crossFade_ = std::max(0.0f, seconds); }
    float CrossFade() const { return crossFade_; }

    /// 一次性动作播完后的自动淡出时长（秒）。
    /// 旧实现播完瞬间把姿态清零，会看到明显「顿一下」；现在平滑回到待机。
    void SetEndFade(float seconds) { endFade_ = std::max(0.0f, seconds); }
    float EndFade() const { return endFade_; }

    /// 是否正处于「动作切换的交叉淡出」阶段（供单测与诊断）。
    bool HasOutgoing() const { return hasOutgoing_; }

    /// 每帧推进并输出骨骼偏移。
    /// 输出是「当前动作 * 其权重 + 上一个动作 * 淡出权重」按骨骼名累加的结果，
    /// 因此动作之间、动作与待机之间都是连续过渡，不会突变。
    ///
    /// @param dt   时间步长（秒）
    /// @param pose 输出：骨骼偏移
    void Tick(float dt, BonePose& pose);

    /// 设置随机待机的间隔范围（秒）。
    /// 与 FULASKA 的「每 8~18 秒随机来一段小动作」一致。
    void SetRandomRange(float minSeconds, float maxSeconds) {
        randomMin_ = minSeconds;
        randomMax_ = maxSeconds;
    }

    /// 设置随机待机的触发概率 [0,1]。
    /// 到达随机间隔后，以该概率决定是否真的播放（避免过于机械）。
    void SetRandomChance(float chance) { randomChance_ = chance; }

    /// 注册一个可用于随机触发的预置动作。
    void AddRandomMotion(const BoneMotion& motion) { randomMotions_.push_back(motion); }

    /// 清空随机动作池。
    void ClearRandomMotions() { randomMotions_.clear(); }

    /// 随机动作池大小。
    std::size_t RandomMotionCount() const { return randomMotions_.size(); }

    /// 立即触发一个随机动作。
    /// @return 触发了返回 true。
    bool TriggerRandom();

private:
    /// 采样指定动作的关键帧，输出骨骼偏移（未乘淡入淡出权重）。
    void Sample(const BoneMotion& motion, float time, BonePose& pose) const;

    /// 关键帧插值：找到 time 两侧的关键帧并线性插值。
    static void Interpolate(const BoneMotion& motion, float time, BonePose& pose);

    /// 把一层采样结果按权重累加进输出（同名骨骼合并，避免产生重复条目）。
    static void Accumulate(BonePose& dst, const BonePose& layer, float weight);

    BoneMotion current_;
    bool playing_ = false;
    float time_ = 0.0f;
    /// 淡入进度 [0,1]。
    float fadeIn_ = 1.0f;
    float fadeInDuration_ = 0.0f;
    /// 淡出进度 [0,1]（1 = 完全淡出）。
    float fadeOut_ = 0.0f;
    float fadeOutDuration_ = 0.0f;

    // --- 交叉淡入淡出：切换动作时旧动作作为「淡出层」继续输出 ---
    BoneMotion outgoing_;
    float outgoingTime_ = 0.0f;
    float outgoingWeight_ = 0.0f;
    float outgoingFade_ = 0.0f;
    float outgoingFadeDuration_ = 0.0f;
    bool hasOutgoing_ = false;
    float crossFade_ = 0.28f;
    /// 一次性动作播完后的自动淡出时长（秒）。
    float endFade_ = 0.22f;

    std::vector<BoneMotion> randomMotions_;
    float randomMin_ = 8.0f;
    float randomMax_ = 18.0f;
    float randomChance_ = 0.7f;
    float randomTimer_ = 6.0f;
    std::size_t randomCursor_ = 0;
};

/// 内置骨骼动作库（VRM-VPet 风格）。
///
/// 动作按「骨骼角色」而不是「具体骨骼名」定义，
/// 由后端把角色映射到实际骨骼（PMX 的「左腕」、VRM 的 upperArm.L 等）。
/// 这样同一套动作能同时用在 PMX 与 VRM 上。
///
/// 骨骼角色：
///   "head"      头
///   "spine"     躯干
///   "chest"     胸
///   "leftArm"   左臂（肩）
///   "rightArm"  右臂（肩）
///   "leftForearm" 左小臂
///   "rightForearm" 右小臂
namespace bone_motions {

/// 点头：头快速下点两下，躯干轻微跟随。
BoneMotion Nod();

/// 摇头：头左右摇，躯干轻微跟随。
BoneMotion Shake();

/// 挥手：抬起右臂（VRM-VPet 的经典招呼）并左右摆动。
///
/// @param arm "rightArm" 或 "leftArm"
BoneMotion Wave(const std::string& arm = "rightArm");

/// 全部内置动作。
std::vector<BoneMotion> Builtin();

/// 按 id 查找内置动作。
/// @return 找不到返回 false。
bool Find(const std::string& id, BoneMotion& out);

}  // namespace bone_motions

}  // namespace ppet::model
