// PPet - C++ 移植版
// 触摸反应变体选择器：把「区域 + 累积器输出」映射成具体反应。
//
// 【为什么需要它】
//   现状的触摸反馈是「区域 → 固定动作」的单层映射：同一区域被反复摸、或
//   交替摸不同区域，反应都一样。选择器在累积器的滑动窗口之上加一层决策：
//     - 同区域反复摸 → 疲劳厌烦（摇头 + 生气）
//     - 交替摸不同区域 → 困惑（摇头 + 惊讶）
//     - 短时间内多次摸 → 加强（放大开心强度）
//     - 单次摸 → 保持现有区域映射
//   它不取代 MotionRecognizer / ResponseForRegion 的纯映射职责，只做「变体选择」。
//
// 【参考设计】claw-sama 分部位触摸反应 + Mood 累积：相同触摸在不同累积状态下
//   产生不同反馈；交替 / 连续触摸各有权重更高的情绪变体。
//
// 【纯逻辑约束】只含 POD 与字符串，不碰 GL / Qt / 模型，可脱离后端单测。
#pragma once

#include <string>
#include <vector>

#include <src/tools/model/TouchAccumulator.hpp>
#include <src/tools/model/TouchResponse.hpp>

namespace ppet::model {

/// 反应变体类别（诊断 / 测试用）。
enum class ReactionKind {
    Normal,      ///< 单次触摸，保持区域默认映射。
    Intensified, ///< 窗口内多次触摸，放大开心强度。
    Fatigued,    ///< 同区域反复触摸，疲劳厌烦。
    Confused,    ///< 交替触摸不同区域，困惑。
};

/// 一次触摸的最终反应（动作 + 临时表情叠层 + 变体类别）。
struct TouchReaction {
    /// 要播放的内置骨骼动作 id（nod / shake / wave / waveLeft）。
    const char* motionId = "nod";
    /// 临时表情叠层（写入 ExpressionDirector::SetMotionWeights）。
    std::vector<std::pair<std::string, float>> expression;
    /// 表情叠层影响强度（0..1），0 表示不叠加表情。
    float influence = 0.0f;
    /// 表情叠层保持时长（秒）。
    float holdSeconds = 0.0f;
    /// 反应变体类别。
    ReactionKind kind = ReactionKind::Normal;
};

/// 反应变体选择器。
///
/// 线程约束：单线程（渲染线程）使用，无需加锁。
class TouchReactionSelector {
public:
    /// 选择一次触摸的反应。region 是本次分区，mod 是累积器刚产出的修饰符，
    /// acc 是累积器本身（用于判定交替触摸）。三者一起决定变体。
    TouchReaction Select(TouchRegion region, const TouchModifier& mod,
                         const TouchAccumulator& acc) const {
        // 基线：区域默认映射（保持 MotionRecognizer / ResponseForRegion 的纯映射职责）。
        const TouchResponse base = ResponseForRegion(TouchRegionName(region));
        TouchReaction r;
        r.motionId = base.motionId;
        r.expression = base.expression;
        r.influence = base.influence;
        r.holdSeconds = base.holdSeconds;
        r.kind = ReactionKind::Normal;

        // 变体优先级：疲劳 > 困惑 > 加强 > 常规。
        if (mod.variant == 2) {
            // 同区域被反复触摸：厌烦（摇头 + 生气），不再无限重复开心。
            r.motionId = "shake";
            r.expression = {{"angry", 0.7f}};
            r.influence = 0.85f;
            r.holdSeconds = 1.2f;
            r.kind = ReactionKind::Fatigued;
        } else if (acc.IsAlternating()) {
            // 交替摸不同区域：困惑（摇头 + 惊讶）。
            r.motionId = "shake";
            r.expression = {{"surprised", 0.6f}};
            r.influence = 0.75f;
            r.holdSeconds = 1.0f;
            r.kind = ReactionKind::Confused;
        } else if (mod.variant == 1) {
            // 短时间内多次触摸：增强开心（强度受累积器封顶约束）。
            r.influence = std::min(1.0f, r.influence * mod.intensity);
            for (auto& kv : r.expression) {
                kv.second = std::min(1.0f, kv.second * mod.intensity);
            }
            r.kind = ReactionKind::Intensified;
        }
        return r;
    }
};

}  // namespace ppet::model
