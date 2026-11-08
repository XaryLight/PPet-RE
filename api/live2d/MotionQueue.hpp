// PPet - C++ 移植版
// Live2D 动作队列：对齐 Cubism Native Framework 的 MotionQueueManager 语义。
//
// 【参考设计：完全基于 C++ 渲染的 Live2D 项目】
//   live2dwallpaper / CubismNativeSamples / CubismNativeFramework 这类项目
//   不套 Chromium / Electron，直接由 C++ 调 Cubism Core 完成参数更新与绘制。
//   它们的动作层有个共同核心 —— **动作队列**：
//     Framework/src/Motion/CubismMotionQueueManager.cpp
//       StartMotion()  : 新动作入队前，把所有已存在动作 SetFadeout(自身 FadeOutTime)
//       DoUpdateMotion(): 按队列顺序逐个 UpdateParameters，后来的动作按 fade 权重
//                        覆盖前面的参数，实现交叉淡入淡出（cross-fade）
//     Framework/src/Motion/CubismMotion.cpp
//       tmpFadeIn  = fadeIn  <= 0 ? 1 : EasingSine((t - fadeInStart) / fadeIn)
//       tmpFadeOut = fadeOut <= 0 ? 1 : EasingSine((endTime - t) / fadeOut)
//       paramWeight = weight * fin * fout
//       v = sourceValue + (value - sourceValue) * paramWeight
//
//   本项目早期只维护「一个当前 clip」，切换动作时旧动作瞬间消失，
//   因此动作衔接会跳变。本模块补上队列与官方 easing 公式。
//
// 【纯逻辑约束】不碰 GL / Cubism / Qt / 文件 IO，可脱离 SDK 单测。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace ppet::live2d {

/// 官方 CubismMath::GetEasingSine：0.5 - 0.5*cos(pi*x)，x 钳到 [0,1]。
/// 与线性插值的区别：起止处斜率为 0，视觉上动作切换没有「折角」。
inline float EasingSine(float x) {
    x = std::min(1.0f, std::max(0.0f, x));
    return 0.5f - 0.5f * std::cos(x * 3.14159265358979323846f);
}

/// 队列里的一条动作。
struct MotionQueueEntry {
    int motionIndex = -1;         ///< 全局动作下标
    float time = 0.0f;            ///< 已播放时间（秒）
    float duration = 0.0f;        ///< 总时长，<=0 表示不自动结束
    float fadeIn = 0.0f;          ///< 动作自带淡入
    float fadeOut = 0.0f;         ///< 动作自带淡出
    float fadeOutSeconds = 0.0f;  ///< 被新动作打断时的淡出时长（官方 SetFadeout）
    float fadeOutStart = 0.0f;    ///< 开始淡出的绝对时刻（官方 StartFadeout）
    bool fadingOut = false;       ///< 已被新动作挤掉，正在淡出
    bool loop = false;            ///< Meta.Loop
    bool finished = false;
    int priority = 0;             ///< 数值越大越优先，见 MotionPriority
    std::string groupName;

    bool IsPrimary() const { return !fadingOut && !finished && motionIndex >= 0; }
};

/// 一条混合输出：调用方按 motioIndex 采样曲线，再以 weight 叠加到参数基线。
struct QueueBlend {
    int motionIndex = -1;
    float time = 0.0f;
    float weight = 0.0f;   ///< [0,1] = EasingSine(fadeIn) * EasingSine(fadeOut)
    int priority = 0;
    std::string groupName;
};

/// 官方 Framework 风格的动作队列。
///
/// 用法（每帧）：
///   queue.Tick(dt);
///   for (const QueueBlend& b : queue.Blends()) { /* 采样并按 weight 叠加 */ }
class MotionQueue {
public:
    void Reset() {
        entries_.clear();
        clock_ = 0.0f;
    }

    /// 启动一个新动作。对齐官方 StartMotion：先把所有已有动作标记为淡出。
    ///
    /// @param now 当前队列时钟（秒），通常传 Clock()。
    void StartMotion(int motionIndex, float duration, float fadeIn, float fadeOut, bool loop,
                     int priority, const std::string& groupName, float now) {
        if (motionIndex < 0) return;
        for (MotionQueueEntry& entry : entries_) {
            if (entry.finished || entry.fadingOut) continue;
            // 官方：SetFadeout(已有动作自身的 GetFadeOutTime())，
            // 没有淡出时间时给它一个极短的值，避免瞬间消失。
            entry.fadingOut = true;
            entry.fadeOutSeconds = entry.fadeOut > 0.0f ? entry.fadeOut : 0.15f;
            entry.fadeOutStart = now;
        }

        MotionQueueEntry entry;
        entry.motionIndex = motionIndex;
        entry.duration = duration;
        entry.fadeIn = fadeIn;
        entry.fadeOut = fadeOut;
        entry.loop = loop;
        entry.priority = priority;
        entry.groupName = groupName;
        entries_.push_back(std::move(entry));

        // 防御：极端情况下（如外部连续 StartMotion）队列不应无限增长。
        if (entries_.size() > 8) entries_.erase(entries_.begin());
    }

    /// 让当前主动作结束（互动动作播完、或控制台 action stop）。
    /// 正在淡出的旧动作保留，让它们自然淡完。
    void FinishPrimary() {
        for (MotionQueueEntry& entry : entries_) {
            if (entry.IsPrimary()) entry.finished = true;
        }
    }

    /// 清空队列（re model / action stop 用）。
    void StopAll() { entries_.clear(); }

    /// 每帧推进。
    void Tick(float deltaTime) {
        const float dt = std::max(0.0f, deltaTime);
        clock_ += dt;
        for (MotionQueueEntry& entry : entries_) {
            if (entry.finished) continue;
            entry.time += dt;
            if (entry.loop && entry.duration > 0.0f) {
                entry.time = std::fmod(entry.time, entry.duration);
            } else if (entry.duration > 0.0f && entry.time >= entry.duration) {
                entry.finished = true;
            }
            if (entry.fadingOut && entry.fadeOutSeconds > 0.0f &&
                clock_ >= entry.fadeOutStart + entry.fadeOutSeconds) {
                entry.finished = true;
            }
        }
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [](const MotionQueueEntry& e) {
                                          return e.finished || e.motionIndex < 0;
                                      }),
                       entries_.end());
    }

    /// 按队列顺序返回所有仍生效的动作及其官方 easing 权重。
    std::vector<QueueBlend> Blends() const {
        std::vector<QueueBlend> out;
        out.reserve(entries_.size());
        for (const MotionQueueEntry& entry : entries_) {
            if (entry.finished || entry.motionIndex < 0) continue;
            const float fin = entry.fadeIn > 0.0f
                                  ? EasingSine(entry.time / entry.fadeIn)
                                  : 1.0f;
            float fout = 1.0f;
            if (entry.fadingOut) {
                fout = entry.fadeOutSeconds > 0.0f
                           ? EasingSine((entry.fadeOutStart + entry.fadeOutSeconds - clock_) /
                                        entry.fadeOutSeconds)
                           : 0.0f;
            } else if (entry.fadeOut > 0.0f && entry.duration > 0.0f) {
                fout = EasingSine((entry.duration - entry.time) / entry.fadeOut);
            }
            const float weight = std::min(1.0f, std::max(0.0f, fin * fout));
            if (weight <= 0.0f) continue;
            QueueBlend blend;
            blend.motionIndex = entry.motionIndex;
            blend.time = entry.time;
            blend.weight = weight;
            blend.priority = entry.priority;
            blend.groupName = entry.groupName;
            out.push_back(std::move(blend));
        }
        return out;
    }

    /// 当前主动作（最后一个未淡出、未结束的 entry）。没有时返回 nullptr。
    const MotionQueueEntry* Primary() const {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->IsPrimary()) return &(*it);
        }
        return nullptr;
    }

    float Clock() const { return clock_; }
    bool Empty() const { return entries_.empty(); }
    std::size_t Size() const { return entries_.size(); }
    const std::vector<MotionQueueEntry>& Entries() const { return entries_; }

private:
    std::vector<MotionQueueEntry> entries_;
    float clock_ = 0.0f;
};

}  // namespace ppet::live2d
