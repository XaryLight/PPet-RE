// PPet - C++ 移植版
// 交互累积器：在「区域 → 动作」的单层映射之上加一层跨触摸的状态。
//
// 【为什么需要它】
//   现状是同一个区域被反复触摸时，反应完全一样 —— 没有疲劳、累积或情绪变化。
//   本模块用滑动窗口统计最近 N 秒的触摸序列，产出修饰符：
//     - intensity：强度倍率（越摸越起劲，封顶）
//     - variant：反应变体（常规 / 加强 / 疲劳厌烦）
//   VrmModel 据此（经 TouchReactionSelector）选择动作、调整表情强度，不必重写调度层。
//
// 【参考设计】claw-sama 的 Mood 系统：Mood 从触摸中累积并反向影响反应。
//
// 【纯逻辑约束】只含 POD 与字符串，不碰 GL / Qt / 模型，可脱离后端单测。
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ppet::model {

/// 触摸分区枚举。与 Model.hpp 的 RegionFromPoint / NormalizedFromRegion
/// 产出的字符串口径一一对应；未知分区退回 Other。
enum class TouchRegion { Other, Head, Body, Left, Right, Leg };

/// 字符串 -> 分区（与 Model.hpp 的 head/body/leg/left/right 口径一致）。
inline TouchRegion TouchRegionFromString(const std::string& region) {
    if (region == "head") return TouchRegion::Head;
    if (region == "body") return TouchRegion::Body;
    if (region == "left") return TouchRegion::Left;
    if (region == "right") return TouchRegion::Right;
    if (region == "leg") return TouchRegion::Leg;
    return TouchRegion::Other;
}

/// 分区 -> 字符串（诊断 / 日志用）。
inline const char* TouchRegionName(TouchRegion region) {
    switch (region) {
        case TouchRegion::Head: return "head";
        case TouchRegion::Body: return "body";
        case TouchRegion::Left: return "left";
        case TouchRegion::Right: return "right";
        case TouchRegion::Leg: return "leg";
        default: return "other";
    }
}

/// 一次触摸的修饰符（给 TouchReactionSelector 选反应变体）。
struct TouchModifier {
    /// 滑动窗口内的有效触摸总次数。
    int totalInWindow = 0;
    /// 本次区域在窗口尾部的连续次数（含本次）。
    int sameRegionStreak = 0;
    /// 表情 / 动作强度倍率，范围 [1.0, maxIntensity]（触摸次数驱动）。
    float intensity = 1.0f;
    /// 反应变体：0=常规 1=加强 2=疲劳/厌烦。
    int variant = 0;
};

/// 滑动窗口触摸累积器。
///
/// 线程约束：单线程（渲染线程）使用，无需加锁。
class TouchAccumulator {
public:
    struct Config {
        /// 统计窗口长度（秒）。
        float windowSeconds = 3.0f;
        /// 防抖：两次有效触摸的最小间隔（秒）。
        float minInterval = 0.15f;
        /// 同一区域连续触摸达到该次数后进入「疲劳」变体。
        int fatigueThreshold = 4;
        /// 窗口内总触摸数达到该次数后进入「加强」变体。
        int intensifyThreshold = 3;
        /// 判定「交替触摸」时检查的最近触摸数。
        int alternationLength = 4;
        /// 强度倍率上限。
        float maxIntensity = 2.0f;
    };

    TouchAccumulator() : config_(Config{}) {}
    explicit TouchAccumulator(Config config) : config_(config) {}

    void SetConfig(const Config& config) { config_ = config; }
    const Config& GetConfig() const { return config_; }

    /// 记录一次触摸并返回当前修饰符。
    /// pressIntensity 是按压强度 / 时长归一化（0..1，缺省 1.0），
    /// 供 CumulativeIntensity 做跨触摸累加，不改变次数驱动的 intensity 倍率。
    /// 间隔小于 minInterval 的触摸不计入统计（但仍返回当前修饰符）。
    TouchModifier Observe(TouchRegion region, float pressIntensity = 1.0f) {
        if (now_ - lastObserve_ >= config_.minInterval) {
            lastObserve_ = now_;
            records_.push_back({region, now_, Clamp01(pressIntensity)});
            Prune();
        }
        return BuildModifier(region);
    }

    /// 字符串重载：内部转成 TouchRegion，兼容旧的 head/body/leg/left/right 口径。
    TouchModifier Observe(const std::string& region, float pressIntensity = 1.0f) {
        return Observe(TouchRegionFromString(region), pressIntensity);
    }

    /// 每帧推进（淘汰过期记录）。
    void Tick(float dt) {
        if (!(dt > 0.0f)) return;
        now_ += dt;
        Prune();
    }

    void Reset() {
        records_.clear();
        now_ = 0.0f;
        lastObserve_ = -1.0e9f;
    }

    /// 窗口内有效触摸数（诊断用）。
    std::size_t Count() const { return records_.size(); }

    /// 最近 seconds 秒内指定区域的触摸次数。
    int CountInRegion(TouchRegion region, float seconds) const {
        if (!(seconds > 0.0f)) return 0;
        const float cutoff = now_ - seconds;
        int count = 0;
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->time < cutoff) break;
            if (it->region == region) ++count;
        }
        return count;
    }

    /// 最近一次有效触摸的区域（无记录时返回 nullopt）。
    std::optional<TouchRegion> LastRegion() const {
        if (records_.empty()) return std::nullopt;
        return records_.back().region;
    }

    /// 窗口内按压强度累加（反映「按得有多用力 / 多频繁」，供反应变体选择）。
    float CumulativeIntensity() const {
        float total = 0.0f;
        for (const auto& r : records_) total += r.intensity;
        return total;
    }

    /// 返回窗口内最近 maxCount 个触摸的区域（时间升序）。
    std::vector<TouchRegion> RecentRegions(int maxCount) const {
        std::vector<TouchRegion> out;
        if (maxCount <= 0) return out;
        const std::size_t start = records_.size() > static_cast<std::size_t>(maxCount)
                                      ? records_.size() - static_cast<std::size_t>(maxCount)
                                      : 0;
        out.reserve(records_.size() - start);
        for (std::size_t i = start; i < records_.size(); ++i) {
            out.push_back(records_[i].region);
        }
        return out;
    }

    /// 是否处于「交替触摸」：窗口尾部最近 alternationLength 次触摸，
    /// 相邻两次区域均不同（head→body→head→body）。用于触发「困惑」变体。
    bool IsAlternating() const {
        const int length = config_.alternationLength;
        if (length < 2) return false;
        if (records_.size() < static_cast<std::size_t>(length)) return false;
        const std::size_t start = records_.size() - static_cast<std::size_t>(length);
        for (std::size_t i = start; i + 1 < records_.size(); ++i) {
            if (records_[i].region == records_[i + 1].region) return false;
        }
        // 至少出现过两个不同区域才算交替，避免单一区域恰好全不同（不可能，但保守）。
        return true;
    }

private:
    struct Record {
        TouchRegion region = TouchRegion::Other;
        float time = 0.0f;
        float intensity = 1.0f;  ///< 按压强度归一化 0..1。
    };

    static float Clamp01(float v) {
        return std::max(0.0f, std::min(1.0f, v));
    }

    void Prune() {
        const float cutoff = now_ - config_.windowSeconds;
        while (!records_.empty() && records_.front().time < cutoff) {
            records_.erase(records_.begin());
        }
    }

    TouchModifier BuildModifier(TouchRegion region) const {
        TouchModifier mod;
        mod.totalInWindow = static_cast<int>(records_.size());
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->region != region) break;
            ++mod.sameRegionStreak;
        }
        const float growth =
            std::max(0.0f, static_cast<float>(mod.totalInWindow - 1)) * 0.15f;
        mod.intensity =
            1.0f + std::min(std::max(0.0f, config_.maxIntensity - 1.0f), growth);
        if (mod.sameRegionStreak >= config_.fatigueThreshold) mod.variant = 2;
        else if (mod.totalInWindow >= config_.intensifyThreshold) mod.variant = 1;
        return mod;
    }

    Config config_;
    float now_ = 0.0f;
    float lastObserve_ = -1.0e9f;
    std::vector<Record> records_;
};

}  // namespace ppet::model
