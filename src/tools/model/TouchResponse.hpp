// PPet - C++ 移植版
// 分区触摸响应：把「摸头 / 摸身体 / 摸左右 / 摸腿」映射到差异化的
// 骨骼动作 + 临时表情叠层。
//
// 【参考设计】AniMate（getanimate.app）VRM 桌宠工坊把互动分为
//   「鼠标靠近 / 点击 / 摸头 / 拖拽」，并强调反馈要明确但不过度频繁。
// 本项目已有的统一区域口径 Model.hpp::RegionFromPoint 产出
//   head/body/leg/left/right 五个分区；本模块为每分区给出一套反馈方案。
//
// 【纯逻辑约束】只含 POD 与字符串常量，不碰 GL / Qt / 模型，
//   可脱离后端单测；表情叠层的实际写入由 VrmModel 用
//   ExpressionDirector::SetMotionWeights 完成。
#pragma once

#include <string>
#include <vector>

namespace ppet::model {

/// 一条分区触摸响应。
struct TouchResponse {
    /// 要播放的内置骨骼动作 id（见 BoneMotion.hpp 的 Builtin：nod/shake/wave/waveLeft）。
    const char* motionId = "nod";
    /// 临时表情叠层的逻辑通道权重（写入 ExpressionDirector::SetMotionWeights）。
    /// 空表示不改表情，只播动作。
    std::vector<std::pair<std::string, float>> expression;
    /// 表情叠层影响强度（0..1），0 表示不叠加表情。
    float influence = 0.0f;
    /// 表情叠层保持时长（秒），到期后由调用方 ClearMotionWeights 淡出回落。
    float holdSeconds = 0.0f;
};

/// 按分区取触摸响应。
///
/// 设计取向（对齐案例「反馈明确、不过度频繁」）：
///   - head（摸头）：点头 + 开心，最亲切的反馈；
///   - left / right：朝对应侧挥手 + 开心，体现左右方向感；
///   - body：摇头 + 轻微惊讶，区分于摸头；
///   - leg / 其它：只点头，不带表情偏移，避免下半身摸也一脸开心。
inline TouchResponse ResponseForRegion(const std::string& region) {
    TouchResponse r;
    if (region == "head") {
        r.motionId = "nod";
        r.expression = {{"happy", 0.85f}};
        r.influence = 0.9f;
        r.holdSeconds = 1.2f;
    } else if (region == "left") {
        r.motionId = "waveLeft";
        r.expression = {{"happy", 0.7f}};
        r.influence = 0.8f;
        r.holdSeconds = 1.0f;
    } else if (region == "right") {
        r.motionId = "wave";
        r.expression = {{"happy", 0.7f}};
        r.influence = 0.8f;
        r.holdSeconds = 1.0f;
    } else if (region == "body") {
        r.motionId = "shake";
        r.expression = {{"surprised", 0.5f}};
        r.influence = 0.6f;
        r.holdSeconds = 0.9f;
    } else {
        // leg / 未知分区：只点头，不动表情。
        r.motionId = "nod";
    }
    return r;
}

}  // namespace ppet::model
