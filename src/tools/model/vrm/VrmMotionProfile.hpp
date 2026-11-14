// PPet - C++ 移植版
// VRM 动作识别（需求 4）：根据模型加载后识别出的 humanoid 骨骼 / glTF 动画 /
// 弹簧骨，判断该 VRM 模型「能做什么基础动作」，并产出可读的能力清单。
//
// 设计要点：
//   - 纯逻辑、零依赖（不碰 GL / Cubism），可单测；
//   - 与 VrmModel 解耦：VrmModel 在加载后把识别到的骨骼信息填进 VrmCapabilities，
//     本模块只负责「从能力推导动作清单与摘要」，便于复用与测试；
//   - 内置骨骼动作（点头/摇头/挥手）只要模型有头/躯干/手臂即可触发，始终可用。
#pragma once

#include <string>
#include <vector>

namespace ppet::vrm {

/// VRM 模型的「动作相关」能力（由 VrmModel 加载后识别并填充）。
struct VrmCapabilities {
    bool hasHead = false;     ///< 识别到 head 骨骼（humanoid）
    bool hasSpine = false;    ///< 识别到 spine / chest 骨骼
    bool hasArms = false;     ///< 左右大臂都识别到（能挥手）
    bool hasLegs = false;     ///< 左右大腿都识别到
    int gltfAnimationCount = 0;  ///< 模型内嵌的 glTF 动画条数（可作待机动画）
    int springBoneCount = 0;     ///< 弹簧骨链数（头发/飘带摇摆）
};

/// VRM 动作能力画像（AnalyzeVrmCapabilities 的产物）。
struct VrmMotionProfile {
    VrmCapabilities caps;
    bool hasBasicGestures = true;   ///< 内置 nod/shake/wave 始终可用
    bool supportsIdle = false;      ///< 能做程序化待机微动 / glTF 待机动画
    std::vector<std::string> availableActions;  ///< 可触发的基础动作 id 列表

    /// 人类可读能力摘要（诊断面板 / 日志用）。
    std::string Summary() const {
        std::string s = "VRM 动作识别：";
        s += supportsIdle ? "待机✓" : "待机✗";
        s += " / 基础动作 ";
        for (std::size_t i = 0; i < availableActions.size(); ++i) {
            if (i) s += ",";
            s += availableActions[i];
        }
        s += " / 骨骼 ";
        s += (caps.hasHead ? "头" : "-");
        s += (caps.hasSpine ? "躯干" : "-");
        s += (caps.hasArms ? "臂" : "-");
        s += (caps.hasLegs ? "腿" : "-");
        s += " / 动画 " + std::to_string(caps.gltfAnimationCount);
        s += " / 弹簧骨 " + std::to_string(caps.springBoneCount);
        return s;
    }
};

/// 从能力推导动作画像。
inline VrmMotionProfile AnalyzeVrmCapabilities(const VrmCapabilities& caps) {
    VrmMotionProfile p;
    p.caps = caps;

    // 只要有人形核心骨骼或内嵌动画，就能做待机微动。
    p.supportsIdle =
        caps.hasHead || caps.hasSpine || caps.gltfAnimationCount > 0;

    // 基础动作：内置骨骼动作始终可用。
    p.availableActions.push_back("nod");     // 点头
    p.availableActions.push_back("shake");   // 摇头
    if (caps.hasArms) p.availableActions.push_back("wave");  // 挥手（需手臂骨骼）

    return p;
}

}  // namespace ppet::vrm
