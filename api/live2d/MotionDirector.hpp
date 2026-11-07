// PPet - C++ 移植版
// Live2D 动作调度：待机循环 + 点击互动 + 优先级 + 淡入淡出混合。
//
// 【为什么单独一层】
//   Cubism Core 只提供 csmUpdateModel 这类数据接口，动作调度（MotionQueueManager /
//   优先级 / fade）属于 Framework 层。本项目不引入 Framework，因此自己实现。
//   参考了两类桌宠的动作组织：
//     · 原 ppet（Electron + pixi-live2d-display）：model3.json 的 Motions 按组，
//       点击命中 HitAreas 触发对应组，动作可打断、播完回落到 idle。
//     · Touhou Desktop Pet / 通用 Cubism 桌宠：Idle 组随机循环（带冷却/权重），
//       TapHead/TapBody/Pinch/Shake 等互动组按优先级插入，FadeIn/FadeOut 平滑过渡。
//
// 【设计要点】
//   - 纯逻辑、零外部依赖（不碰 GL / Cubism / 文件 IO），可脱离 SDK 做单元测试，
//     这正是本项目其它纯函数模块（ClampRectInside / AnalyzeFramePixels）的惯例。
//   - 把「选哪个动作」与「把曲线值写进参数」解耦：本类只负责调度与混合权重，
//     Live2DModel 负责采样曲线并写入 Cubism 参数。
//   - 动作组按名字分类（大小写不敏感）：Idle=待机，Tap*/Pinch*/Shake*/Flick*=互动，
//     含 Idle 关键字的组优先作为待机组，其余互动组按 HitArea 名字匹配。
//   - 命中区（HitAreas）来自 model3.json，缺省时用「包围盒空间分区」回退：
//     上=头 / 中=身 / 下=腿 / 左=左手 / 右=右手，与 Cubism 桌宠通用做法一致。
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace ppet::live2d {

/// 动作用途分类。
enum class MotionGroupKind {
    Idle,         ///< 待机组（如 "Idle"），随机循环播放
    Tap,          ///< 点击互动（如 "TapHead" / "TapBody"）
    Pinch,        ///< 捏合互动
    Shake,        ///< 摇晃互动
    Flick,        ///< 轻拂互动
    Expression,   ///< 表情组（如 "Exp"），低优先级，随机点缀
    Other,        ///< 其它/未知组
};

/// 互动优先级：数值越大越优先，仅更高优先级可以打断当前互动动作。
enum class MotionPriority {
    Idle = 0,       ///< 待机，最低
    Expression = 1, ///< 表情点缀
    Normal = 2,     ///< 普通互动（Tap / Pinch / Shake / Flick）
    Force = 3,      ///< 强制互动（用户显式触发，必定打断）
};

/// 依据动作组名推断用途。纯函数，大小写不敏感。
inline MotionGroupKind ClassifyMotionGroup(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) lower.push_back(static_cast<char>(std::tolower(c)));
    if (lower.find("idle") != std::string::npos) return MotionGroupKind::Idle;
    if (lower.find("tap") != std::string::npos) return MotionGroupKind::Tap;
    if (lower.find("pinch") != std::string::npos) return MotionGroupKind::Pinch;
    if (lower.find("shake") != std::string::npos) return MotionGroupKind::Shake;
    if (lower.find("flick") != std::string::npos) return MotionGroupKind::Flick;
    if (lower.find("exp") != std::string::npos) return MotionGroupKind::Expression;
    return MotionGroupKind::Other;
}

/// 从动作组名提取关联的命中区（HitArea）名，用于把点击位置映射到动作组。
/// 例如 "TapHead" -> "head"、"PinchBody" -> "body"。找不到返回空串。
/// 命中区名字通常出现在 model3.json 的 HitAreas[].Name（如 "Head"/"Body"）。
inline std::string HitAreaFromGroupName(const std::string& groupName,
                                        MotionGroupKind kind) {
    if (kind != MotionGroupKind::Tap && kind != MotionGroupKind::Pinch &&
        kind != MotionGroupKind::Shake && kind != MotionGroupKind::Flick) {
        return std::string();
    }
    std::string lower;
    lower.reserve(groupName.size());
    for (unsigned char c : groupName) lower.push_back(static_cast<char>(std::tolower(c)));
    // 去掉动作类型前缀，剩下的即部位名。
    const char* prefixes[] = {"tap", "pinch", "shake", "flick"};
    for (const char* p : prefixes) {
        const std::size_t pos = lower.find(p);
        if (pos != std::string::npos) {
            std::string rest = lower.substr(pos + std::strlen(p));
            // 去掉可能的分隔符/编号（如 "tap_head" -> "head"、"tapbody1" 保留 body1）。
            while (!rest.empty() && (rest.front() == '_' || rest.front() == '-' ||
                                    rest.front() == ' ')) {
                rest.erase(rest.begin());
            }
            return rest;
        }
    }
    return std::string();
}

/// 一个动作组（已加载好的动作在全局 motions 数组中的下标）。
struct MotionGroup {
    std::string name;
    MotionGroupKind kind = MotionGroupKind::Other;
    std::string hitArea;              ///< 关联命中区（可能为空）
    std::vector<int> motionIndices;   ///< 全局动作下标
    bool IsIdle() const { return kind == MotionGroupKind::Idle; }
    bool IsInteraction() const {
        return kind == MotionGroupKind::Tap || kind == MotionGroupKind::Pinch ||
               kind == MotionGroupKind::Shake || kind == MotionGroupKind::Flick;
    }
};

/// 当前正在播放的动作片段。
struct ActiveClip {
    int motionIndex = -1;            ///< 全局动作下标，-1 = 无
    float time = 0.0f;               ///< 已播放时间（秒）
    float duration = 0.0f;           ///< 动作总时长
    float fadeIn = 0.0f;             ///< 淡入时长
    float fadeOut = 0.0f;            ///< 淡出时长
    MotionPriority priority = MotionPriority::Idle;
    bool loop = false;
    std::string groupName;
};

/// 一次动作混合的权重（用于与待机基线/上一个动作做插值）。
struct MotionBlend {
    int motionIndex = -1;
    float time = 0.0f;
    float weight = 0.0f;   ///< [0,1]：fade 综合权重，1 = 完全生效
};

/// 命中区分区（包围盒空间回退）。
/// 输入 nx, ny 为窗口内归一化坐标 [0,1]，原点左上、y 向下（与 GLFW 光标一致）。
/// 返回分区名，供与 HitArea Name 做包含匹配。
inline std::string HitRegionFromPoint(float nx, float ny) {
    if (nx < 0.25f) return "left";
    if (nx > 0.75f) return "right";
    if (ny < 0.28f) return "head";
    if (ny > 0.72f) return "leg";
    return "body";
}

/// 动作调度器。所有方法线程不安全，仅在渲染主线程调用。
class MotionDirector {
public:
    /// 配置动作组与命中区。motions 数组长度用于校验下标；durations/fades 由
    /// 外部通过 SetMotionMeta() 提供（本类不直接依赖 Motion 结构）。
    void Configure(std::vector<MotionGroup> groups, std::vector<std::string> hitAreas,
                   std::size_t motionCount) {
        groups_ = std::move(groups);
        hitAreas_ = std::move(hitAreas);
        motionCount_ = motionCount;
        // 稳定随机源：与 Live2DModel 旧实现一致的固定种子，行为可复现。
        rng_.seed(20240101u);
        Reset();
        // 不在这里选首个待机动作：动作时长/淡入淡出由随后的 SetMotionMeta()
        // 提供，若在此时 StartClip 会读到空元数据，导致首个动作永不结束。
        // 推迟到首次 Tick（那时元数据已就位，且 idleCooldown_=0 会立即起动作）。
    }

    /// 设置每个动作的时长与淡入淡出（与全局 motions 下标一一对应）。
    /// 未调用时按「时长 0（不自动结束）、无 fade」处理。
    void SetMotionMeta(const std::vector<float>& durations,
                       const std::vector<float>& fadeIns,
                       const std::vector<float>& fadeOuts) {
        durations_ = durations;
        fadeIns_ = fadeIns;
        fadeOuts_ = fadeOuts;
    }

    /// 每帧推进。返回当前应生效的动作（可能为空，表示纯待机）。
    /// 规则：
    ///   - 互动动作播放中：推进时间，结束后回落到待机（重新挑 Idle）。
    ///   - 待机动作播放中：推进时间，结束后进入冷却（idleCooldown_），
    ///     冷却结束再随机挑下一个 Idle 动作。
    ///   - 互动动作可以打断待机动作；更高优先级互动可以打断更低优先级互动。
    void Tick(float deltaTime) {
        const float dt = std::max(0.0f, deltaTime);
        if (current_.motionIndex >= 0) {
            current_.time += dt;
            const float dur = current_.duration;
            if (current_.loop && dur > 0.0f) {
                current_.time = std::fmod(current_.time, dur);
            } else if (dur > 0.0f && current_.time >= dur) {
                FinishClip();
                // 结束当帧不再扣冷却 / 起新动作：否则一次大 dt（卡顿帧）会用
                // 同一个 dt 把刚设置的冷却一并透支，破坏「待机间隔」的语义。
                return;
            }
        }
        if (current_.motionIndex < 0) {
            if (idleCooldown_ > 0.0f) {
                idleCooldown_ -= dt;
                if (idleCooldown_ > 0.0f) return;
            }
            PickNextIdle();
        }
    }

    /// 用户点击互动。nx/ny 为窗口内归一化坐标 [0,1]。
    /// 命中区匹配优先：先按分区名找 HitArea，再找关联互动动作组；
    /// 找不到时退回任意互动组；仍没有则触发一次表情/待机点缀。
    /// @return 是否成功触发了互动动作。
    bool Interact(float nx, float ny, MotionPriority priority = MotionPriority::Normal) {
        const std::string region = HitRegionFromPoint(nx, ny);
        std::string hitName = MatchHitArea(region);

        // 1) 优先：关联命中区的互动组。
        std::vector<const MotionGroup*> candidates;
        if (!hitName.empty()) {
            for (const MotionGroup& g : groups_) {
                if (g.IsInteraction() && SameArea(g.hitArea, hitName)) {
                    candidates.push_back(&g);
                }
            }
        }
        // 2) 退化：分区名直接出现在组名里（如组名 "TapHead" 但无 HitAreas 定义）。
        if (candidates.empty()) {
            for (const MotionGroup& g : groups_) {
                if (g.IsInteraction() && !region.empty() &&
                    ContainsIgnoreCase(g.name, region)) {
                    candidates.push_back(&g);
                }
            }
        }
        // 3) 再退化：任意互动组（模型只定义了 TapBody 之类）。
        if (candidates.empty()) {
            for (const MotionGroup& g : groups_) {
                if (g.IsInteraction()) candidates.push_back(&g);
            }
        }
        // 4) 最后：表情组点缀，保证「点击有反应」。
        if (candidates.empty()) {
            for (const MotionGroup& g : groups_) {
                if (g.kind == MotionGroupKind::Expression) candidates.push_back(&g);
            }
        }
        if (candidates.empty()) return false;

        // 优先级：当前正在播放更高优先级互动时，普通触发不打断。
        if (current_.motionIndex >= 0 &&
            current_.priority > MotionPriority::Expression &&
            priority < current_.priority) {
            return false;
        }
        if (priority == MotionPriority::Normal &&
            current_.priority == MotionPriority::Normal && current_.time < 0.15f) {
            // 极短时间内的重复点击合并，避免抖动式重播。
            return false;
        }

        std::uniform_int_distribution<std::size_t> groupDist(0, candidates.size() - 1);
        const MotionGroup* group = candidates[groupDist(rng_)];
        if (!group || group->motionIndices.empty()) return false;
        std::uniform_int_distribution<std::size_t> motionDist(0, group->motionIndices.size() - 1);
        StartClip(group->motionIndices[motionDist(rng_)], priority, group->name);
        return true;
    }

    /// 直接按动作组名触发（供托盘/测试用）。
    bool PlayGroup(const std::string& groupName,
                   MotionPriority priority = MotionPriority::Force) {
        for (const MotionGroup& g : groups_) {
            if (g.name != groupName || g.motionIndices.empty()) continue;
            std::uniform_int_distribution<std::size_t> dist(0, g.motionIndices.size() - 1);
            StartClip(g.motionIndices[dist(rng_)], priority, g.name);
            return true;
        }
        return false;
    }

    /// 当前动作的混合输出。weight 综合了淡入与淡出。
    MotionBlend Blend() const {
        MotionBlend blend;
        if (current_.motionIndex < 0) return blend;
        blend.motionIndex = current_.motionIndex;
        blend.time = current_.time;
        blend.weight = FadeWeight();
        return blend;
    }

    const ActiveClip& Current() const { return current_; }
    bool HasInteractionGroups() const {
        for (const MotionGroup& g : groups_) {
            if (g.IsInteraction()) return true;
        }
        return false;
    }
    bool HasIdleGroups() const {
        for (const MotionGroup& g : groups_) {
            if (g.IsIdle() && !g.motionIndices.empty()) return true;
        }
        return false;
    }
    std::size_t GroupCount() const { return groups_.size(); }
    std::size_t HitAreaCount() const { return hitAreas_.size(); }
    const std::vector<MotionGroup>& Groups() const { return groups_; }
    const std::vector<std::string>& HitAreas() const { return hitAreas_; }

    /// 供诊断面板显示的一行状态。
    std::string StatusText() const {
        if (current_.motionIndex < 0) {
            return HasIdleGroups() ? "待机（等待下一个 Idle 动作）" : "无动作";
        }
        std::string out = current_.priority >= MotionPriority::Normal ? "互动动作：" : "待机动作：";
        out += current_.groupName.empty() ? ("#" + std::to_string(current_.motionIndex))
                                          : current_.groupName;
        out += " " + std::to_string(static_cast<int>(current_.time * 10) / 10.0f) + "s";
        return out;
    }

    /// 待机动作之间的冷却（秒）与随机抖动范围。参考通用桌宠的 3~7 秒间隔。
    void SetIdleCooldown(float base, float jitter) {
        idleCooldownBase_ = std::max(0.0f, base);
        idleCooldownJitter_ = std::max(0.0f, jitter);
    }

private:
    void Reset() {
        current_ = ActiveClip();
        idleCooldown_ = 0.0f;
    }

    float MetaAt(const std::vector<float>& v, std::size_t i) const {
        return i < v.size() ? std::max(0.0f, v[i]) : 0.0f;
    }

    void StartClip(int motionIndex, MotionPriority priority, const std::string& groupName) {
        if (motionIndex < 0 || static_cast<std::size_t>(motionIndex) >= motionCount_) return;
        current_ = ActiveClip();
        current_.motionIndex = motionIndex;
        current_.duration = MetaAt(durations_, static_cast<std::size_t>(motionIndex));
        current_.fadeIn = MetaAt(fadeIns_, static_cast<std::size_t>(motionIndex));
        current_.fadeOut = MetaAt(fadeOuts_, static_cast<std::size_t>(motionIndex));
        current_.priority = priority;
        current_.groupName = groupName;
    }

    void FinishClip() {
        const bool wasInteraction = current_.priority >= MotionPriority::Normal;
        current_ = ActiveClip();
        if (wasInteraction) {
            // 互动结束：短冷却后回到待机，不要立刻又随机起互动。
            idleCooldown_ = 0.2f;
        } else {
            idleCooldown_ = NextIdleCooldown();
        }
    }

    float NextIdleCooldown() {
        std::uniform_real_distribution<float> dist(0.0f, idleCooldownJitter_);
        return idleCooldownBase_ + dist(rng_);
    }

    void PickNextIdle() {
        if (!HasIdleGroups()) return;
        std::vector<const MotionGroup*> idleGroups;
        for (const MotionGroup& g : groups_) {
            if (g.IsIdle() && !g.motionIndices.empty()) idleGroups.push_back(&g);
        }
        std::uniform_int_distribution<std::size_t> groupDist(0, idleGroups.size() - 1);
        const MotionGroup* group = idleGroups[groupDist(rng_)];
        std::uniform_int_distribution<std::size_t> motionDist(0, group->motionIndices.size() - 1);
        StartClip(group->motionIndices[motionDist(rng_)], MotionPriority::Idle, group->name);
        current_.loop = false;  // 待机动作靠冷却循环，不自己在末尾循环
    }

    /// 淡入淡出综合权重。淡出只在动作有有限时长时生效。
    float FadeWeight() const {
        if (current_.motionIndex < 0) return 0.0f;
        const float t = current_.time;
        float w = 1.0f;
        if (current_.fadeIn > 0.0f && t < current_.fadeIn) {
            w = std::min(w, t / current_.fadeIn);
        }
        if (current_.fadeOut > 0.0f && current_.duration > 0.0f) {
            const float remain = current_.duration - t;
            if (remain < current_.fadeOut) {
                w = std::min(w, std::max(0.0f, remain / current_.fadeOut));
            }
        }
        return std::min(1.0f, std::max(0.0f, w));
    }

    /// 分区名 -> HitArea Name 匹配（大小写不敏感、双向包含）。
    std::string MatchHitArea(const std::string& region) const {
        if (region.empty()) return std::string();
        for (const std::string& area : hitAreas_) {
            if (SameArea(area, region)) return area;
        }
        return std::string();
    }
    static bool SameArea(const std::string& a, const std::string& b) {
        if (a.empty() || b.empty()) return false;
        return ContainsIgnoreCase(a, b) || ContainsIgnoreCase(b, a);
    }
    static bool ContainsIgnoreCase(const std::string& hay, const std::string& needle) {
        std::string h, n;
        h.reserve(hay.size());
        n.reserve(needle.size());
        for (unsigned char c : hay) h.push_back(static_cast<char>(std::tolower(c)));
        for (unsigned char c : needle) n.push_back(static_cast<char>(std::tolower(c)));
        return h.find(n) != std::string::npos;
    }

    std::vector<MotionGroup> groups_;
    std::vector<std::string> hitAreas_;
    std::vector<float> durations_;
    std::vector<float> fadeIns_;
    std::vector<float> fadeOuts_;
    std::size_t motionCount_ = 0;
    std::mt19937 rng_{20240101u};
    ActiveClip current_;
    float idleCooldown_ = 0.0f;
    float idleCooldownBase_ = 3.0f;
    float idleCooldownJitter_ = 4.0f;
};

}  // namespace ppet::live2d
