// PPet - C++ 移植版
// VRM 动作调度器：动作源仲裁 + 回退链。
//
// 【为什么需要它】
//   VrmModel 里「程序化待机 vs glTF 内嵌动画」目前是硬编码二选一
//   （`if (hasAnimation_) return;`），且 glTF 动画存在时连触摸内置骨骼动作都被短路。
//   本模块对标 Live2D 的 MotionDirector（api/live2d/MotionDirector.hpp），把
//   「选哪个动作源」从 VrmModel 里抽成纯逻辑调度：
//     - 注册动作源（按类别 + 优先级）；
//     - 依据行为状态选择动作源并给出回退链；
//     - 回退链：外部源（glTF / VRMA）优先，缺失时回退程序化待机 / 内置骨骼动作。
//
// 【回退链】
//   触摸/交互（Reacting / Interacting）：Touch 源存在 → 播放外部触摸动画；
//     缺失 → 回退内置骨骼动作（nod / shake / wave）。
//   待机等其余状态：Idle 源存在 → 播放外部待机动画（glTF / VRMA 循环）；
//     缺失 → 回退程序化待机（呼吸 / 重心 / 随机小动作）。
//
// 【与 Live2D MotionDirector 的对应】
//   - MotionGroup / MotionGroupKind  ->  VrmMotionCategory（Idle / Touch / Emote）
//   - 优先级仲裁                        ->  PickBest（同类取优先级最高）
//   - 命中区 → 动作组                   ->  由 TouchReactionSelector 负责（缺陷 2）
//   - 交叉淡入淡出                      ->  由 BoneMotionPlayer / 采样层负责
//
// 【纯逻辑约束】只含 POD 与字符串，不碰 GL / Qt / 模型 / 文件 IO，可脱离后端单测。
#pragma once

#include <src/tools/model/BehaviorStateMachine.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ppet::vrm {

/// 动作源类别。
enum class VrmMotionCategory {
    Idle,   ///< 待机循环（glTF 内嵌动画 / VRMA 待机）
    Touch,  ///< 触摸反应（VRMA 触摸动画）
    Emote,  ///< 情绪动作（VRMA 表情动画）
};

/// 类别中文名（诊断 / 日志用）。
inline const char* VrmMotionCategoryName(VrmMotionCategory category) {
    switch (category) {
        case VrmMotionCategory::Idle: return "待机";
        case VrmMotionCategory::Touch: return "触摸";
        case VrmMotionCategory::Emote: return "情绪";
        default: return "其它";
    }
}

/// 一个可被调度的动作源。
struct VrmMotionSource {
    VrmMotionCategory category = VrmMotionCategory::Idle;
    int priority = 0;        ///< 数值越大越优先，同类取最高。
    std::string id;          ///< 诊断 id，如 "gltf" / "vrma:walk"。
    bool loop = false;       ///< 是否循环播放。
};

/// 一次调度的决策结果。
struct VrmMotionDecision {
    bool useExternal = false;        ///< 是否使用外部动作源。
    int sourceIndex = -1;            ///< 选中的外部源下标（useExternal 时有效）。
    VrmMotionCategory category = VrmMotionCategory::Idle;  ///< 本次调度类别。
    bool fallbackProcedural = false; ///< 是否回退程序化（待机 / 内置骨骼动作）。
};

/// VRM 动作调度器。线程不安全，仅在渲染主线程调用。
class VrmMotionDirector {
public:
    /// 注册一个动作源，返回其下标。
    int RegisterSource(VrmMotionSource source) {
        sources_.push_back(std::move(source));
        return static_cast<int>(sources_.size()) - 1;
    }

    /// 清空全部动作源（切换 / 重载模型时调用）。
    void ClearSources() { sources_.clear(); }

    /// 已注册的源数量。
    std::size_t SourceCount() const { return sources_.size(); }

    /// 是否存在某类别的外部源。
    bool HasCategory(VrmMotionCategory category) const {
        return PickBest(category) >= 0;
    }

    /// 依据行为状态选择动作来源。
    ///
    /// 触摸/交互状态走 Touch 类（回退内置骨骼动作），其余状态走 Idle 类
    /// （回退程序化待机）。同类源按优先级取最高。
    VrmMotionDecision Select(model::BehaviorState state) const {
        const bool interactive = state == model::BehaviorState::Reacting ||
                                 state == model::BehaviorState::Interacting;
        const VrmMotionCategory want =
            interactive ? VrmMotionCategory::Touch : VrmMotionCategory::Idle;

        VrmMotionDecision decision;
        decision.category = want;
        const int index = PickBest(want);
        if (index >= 0) {
            decision.useExternal = true;
            decision.sourceIndex = index;
        } else {
            decision.fallbackProcedural = true;
        }
        return decision;
    }

    /// 便捷：当前状态是否应由外部待机动画接管（供 VrmModel::UpdateIdle 短路）。
    bool UseExternalIdle(model::BehaviorState state) const {
        const VrmMotionDecision d = Select(state);
        return d.useExternal && d.category == VrmMotionCategory::Idle;
    }

    /// 便捷：当前状态是否有外部触摸动画（供 VrmModel::TriggerTouch 仲裁）。
    bool UseExternalTouch(model::BehaviorState state) const {
        const VrmMotionDecision d = Select(state);
        return d.useExternal && d.category == VrmMotionCategory::Touch;
    }

private:
    /// 从某类别里选优先级最高的源（并列时取先注册者）。
    int PickBest(VrmMotionCategory category) const {
        int best = -1;
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            if (sources_[i].category != category) continue;
            if (best < 0 ||
                sources_[i].priority > sources_[static_cast<std::size_t>(best)].priority) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    std::vector<VrmMotionSource> sources_;
};

}  // namespace ppet::vrm
