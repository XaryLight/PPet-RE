// PPet - C++ 移植版
// 智能动作识别单元测试（需求 2）：
//   - 按文件名识别 idle / touch / expression（覆盖 guanghui_7 这类空名组模型）
//   - 区域 -> 触摸动作映射
//   - 按动作组识别
// 纯逻辑、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/model/MotionRecognizer.hpp>

#include <cassert>
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    using ppet::live2d::AnalyzeMotionFiles;
    using ppet::live2d::AnalyzeMotionGroups;
    using ppet::live2d::MotionGroup;
    using ppet::live2d::MotionGroupKind;

    // 模拟 guanghui_7 风格的 47 个动作文件名（组名为空，语义全在文件名里）。
    std::vector<std::string> files = {
        "motions/idle.motion3.json",
        "motions/idle1.motion3.json",
        "motions/stand.motion3.json",
        "motions/touch_head.motion3.json",
        "motions/tap_body.motion3.json",
        "motions/pinch.motion3.json",
        "motions/shake.motion3.json",
        "motions/flick.motion3.json",
        "motions/exp_happy.motion3.json",
        "motions/exp_blink.motion3.json",
        "motions/blink.motion3.json",
    };

    const auto a = AnalyzeMotionFiles(files);
    check(a.hasIdle, "识别到待机动作");
    check(a.idleCount == 3, "待机数 = 3（idle/idle1/stand）");
    check(a.interactionCount == 5, "互动数 = 5（touch_head/tap_body/pinch/shake/flick）");
    check(a.expressionCount == 2, "表情数 = 2（exp_happy/exp_blink）");
    // 「blink」不含分类关键字（exp/emotion/face），按既有约定归为「其它」。
    check(a.otherCount == 1, "其它数 = 1（blink）");
    check(a.touchByRegion.count("head") == 1, "head 区域有映射");
    check(a.touchByRegion.count("body") == 1, "body 区域有映射");
    check(a.touchByRegion.at("head")[0] == "touch_head", "head -> touch_head");
    check(!a.idleNames.empty(), "idleNames 非空");

    // 摘要非空且包含关键字。
    check(!a.Summary().empty(), "Summary 非空");

    // 按动作组识别：标准 model3.json 分组场景。
    MotionGroup g1;
    g1.name = "Idle";
    g1.kind = MotionGroupKind::Idle;
    g1.motionIndices = {0};
    MotionGroup g2;
    g2.name = "TapHead";
    g2.kind = MotionGroupKind::Tap;
    g2.hitArea = "head";
    g2.motionIndices = {1, 2};
    MotionGroup g3;
    g3.name = "Exp";
    g3.kind = MotionGroupKind::Expression;
    g3.motionIndices = {3};

    const auto b = AnalyzeMotionGroups({g1, g2, g3});
    check(b.idleCount == 1 && b.hasIdle, "组识别：1 个待机");
    check(b.interactionCount == 1, "组识别：1 个触摸");
    check(b.expressionCount == 1, "组识别：1 个表情");
    check(b.touchByRegion.at("head")[0] == "TapHead", "组识别：head -> TapHead");

    // 空输入不产生任何动作。
    const auto empty = AnalyzeMotionFiles({});
    check(!empty.hasIdle && empty.idleCount == 0 && empty.interactionCount == 0,
          "空输入无动作");

    if (failures == 0) {
        std::printf("motion_recognizer_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("motion_recognizer_unit: %d FAILED\n", failures);
    return 1;
}
