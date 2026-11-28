// PPet - C++ 移植版
// 交互累积器：在「区域 → 动作」的单层映射之上加一层跨触摸的状态。
//
// 【为什么需要它】
//   现状是同一个区域被反复触摸时，反应完全一样 —— 没有疲劳、累积或情绪变化。
//   本模块用滑动窗口统计最近 N 秒的触摸序列，产出修饰符：
//     - intensity：强度倍率（越摸越起劲，封顶）
//     - variant：反应变体（常规 / 加强 / 疲劳厌烦）
//   VrmModel 据此选择动作、调整表情强度，不必重写调度层。
//
// 【参考设计】claw-sama 的 Mood 系统：Mood 从触摸中累积并反向影响反应。
//
// 【纯逻辑约束】只含 POD 与字符串，不碰 GL / Qt / 模型，可脱离后端单测。
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace ppet::model {

/// 一次触摸的修饰符（给 VrmModel 选反应变体）。
struct TouchModifier {
    /// 滑动窗口内的有效触摸总次数。
    int totalInWindow = 0;
    /// 本次区域在窗口尾部的连续次数（含本次）。
    int sameRegionStreak = 0;
    /// 表情 / 动作强度倍率，范围 [1.0, maxIntensity]。
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
        /// 强度倍率上限。
        float maxIntensity = 2.0f;
    };

    TouchAccumulator() : config_(Config{}) {}
    explicit TouchAccumulator(Config config) : config_(config) {}

    void SetConfig(const Config& config) { config_ = config; }
    const Config& GetConfig() const { return config_; }

    /// 记录一次触摸并返回当前修饰符。
    /// 间隔小于 minInterval 的触摸不计入统计（但仍返回当前修饰符）。
    TouchModifier Observe(const std::string& region) {
        if (now_ - lastObserve_ >= config_.minInterval) {
            lastObserve_ = now_;
            records_.push_back({region, now_});
            Prune();
        }
        return BuildModifier(region);
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

private:
    struct Record {
        std::string region;
        float time = 0.0f;
    };

    void Prune() {
        const float cutoff = now_ - config_.windowSeconds;
        while (!records_.empty() && records_.front().time < cutoff) {
            records_.erase(records_.begin());
        }
    }

    TouchModifier BuildModifier(const std::string& region) const {
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
        else if (mod.totalInWindow >= 3) mod.variant = 1;
        return mod;
    }

    Config config_;
    float now_ = 0.0f;
    float lastObserve_ = -1.0e9f;
    std::vector<Record> records_;
};

}  // namespace ppet::model
