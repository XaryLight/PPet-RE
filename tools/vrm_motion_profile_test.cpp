// PPet - C++ 移植版
// VRM 动作识别单元测试（需求 4）：
//   - 从 humanoid 骨骼 / glTF 动画 / 弹簧骨推导「该 VRM 模型能做什么基础动作」
//   - 内置 nod / shake 始终可用；wave 仅在识别到双臂时出现
//   - supportsIdle 的判定（有人形核心骨骼或内嵌动画即可待机）
//   - Summary 摘要内容
// 纯逻辑、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/model/vrm/VrmMotionProfile.hpp>

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    using ppet::vrm::AnalyzeVrmCapabilities;
    using ppet::vrm::VrmCapabilities;

    // --- 完整人形：头 + 躯干 + 双臂 + 双腿 + 内嵌动画 + 弹簧骨 ---
    VrmCapabilities full;
    full.hasHead = true;
    full.hasSpine = true;
    full.hasArms = true;
    full.hasLegs = true;
    full.gltfAnimationCount = 3;
    full.springBoneCount = 2;

    auto p = AnalyzeVrmCapabilities(full);
    check(p.supportsIdle, "完整人形：支持待机");
    check(p.hasBasicGestures, "基础手势始终可用");
    check(p.availableActions.size() == 3, "有 3 个基础动作（nod/shake/wave）");
    check(p.availableActions[0] == "nod", "动作 0 = nod");
    check(p.availableActions[1] == "shake", "动作 1 = shake");
    check(p.availableActions[2] == "wave", "动作 2 = wave（有手臂）");

    const std::string sFull = p.Summary();
    check(!sFull.empty(), "Summary 非空");
    check(sFull.find("待机✓") != std::string::npos, "摘要含 待机✓");
    check(sFull.find("wave") != std::string::npos, "摘要含 wave");
    check(sFull.find("头") != std::string::npos, "摘要含 头 骨骼标记");
    check(sFull.find("动画 3") != std::string::npos, "摘要含 动画 3");
    check(sFull.find("弹簧骨 2") != std::string::npos, "摘要含 弹簧骨 2");

    // --- 无手臂：挥手不可用 ---
    VrmCapabilities noArms = full;
    noArms.hasArms = false;
    auto p2 = AnalyzeVrmCapabilities(noArms);
    check(p2.availableActions.size() == 2, "无手臂：只有 2 个动作（nod/shake）");
    check(p2.availableActions[0] == "nod" && p2.availableActions[1] == "shake",
          "无手臂：动作仍为 nod/shake");
    const std::string sNoArms = p2.Summary();
    check(sNoArms.find("wave") == std::string::npos, "无手臂：摘要不含 wave");
    check(sNoArms.find("臂") == std::string::npos, "无手臂：摘要不含 臂 标记");
    check(sNoArms.find("-") != std::string::npos, "无手臂：骨骼缺失处显示 -");

    // --- 只有头部、无任何动画：仍可做程序化待机（呼吸/微动） ---
    VrmCapabilities headOnly;
    headOnly.hasHead = true;  // 仅头部
    auto p3 = AnalyzeVrmCapabilities(headOnly);
    check(p3.supportsIdle, "仅头部也支持待机（程序化微动）");
    check(p3.availableActions.size() == 2, "仅头部：2 个动作（无挥手）");

    // --- 空能力：没有任何骨骼也没有动画，不支持待机 ---
    VrmCapabilities empty;
    auto p4 = AnalyzeVrmCapabilities(empty);
    check(!p4.supportsIdle, "空能力：不支持待机");
    check(p4.availableActions.size() == 2, "空能力：基础手势仍列出 nod/shake");
    const std::string sEmpty = p4.Summary();
    check(sEmpty.find("待机✗") != std::string::npos, "摘要含 待机✗");

    if (failures == 0) {
        std::printf("vrm_motion_profile_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("vrm_motion_profile_unit: %d FAILED\n", failures);
    return 1;
}
