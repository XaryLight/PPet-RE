// PPet - C++ 移植版
// PMX/VRM 表情管理（ExpressionDirector.hpp）单元测试。
//
// 参考 VRM-VPet 的 ExpressionManager：预设 + 别名解析 + smoothstep 过渡 +
// 自动眨眼独立叠层 + 动作表情临时叠层淡出 + 二元表情量化。
//
// 纯逻辑、零 GL/Qt 依赖，可脱离模型后端单测。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <src/tools/model/ExpressionDirector.hpp>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

bool Nearly(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

using namespace ppet;

// 构造一个带 happy/blink 通道的模型（模拟 VRM 真实表情名）。
std::vector<std::string> SampleChannels() {
    return {"happy", "sad", "angry", "blink", "blinkLeft", "blinkRight",
            "aa", "ih", "ou", "ee", "oh"};
}

void TestAliasResolve() {
    std::printf("-- 别名解析 Resolve --\n");
    ExpressionDirector d;
    d.Bind(SampleChannels());
    Check(d.Resolve("happy") == 0, "happy -> 0");
    Check(d.Resolve("Happy") == 0, "Happy 大小写不敏感");
    Check(d.Resolve("joy") == 0, "joy 命中 happy 别名");
    Check(d.Resolve("笑い") == 0, "笑い 命中 happy 中文/日文别名");
    Check(d.Resolve("blink") == 3, "blink -> 3");
    Check(d.Resolve("winkleft") == 4, "winkleft 命中 blinkLeft 别名");
    Check(d.Resolve("不存在") == -1, "不存在通道 -> -1");
    Check(d.Resolve("") == -1, "空名 -> -1");
}

void TestPresetApplyAndTransition() {
    std::printf("-- 预设应用与过渡 --\n");
    ExpressionDirector d;
    d.Bind(SampleChannels());
    Check(d.ApplyPreset("happy"), "ApplyPreset(happy) 成功");
    Check(d.ActivePreset() == "happy", "当前预设 = happy");
    // 过渡刚开始时 base_ 仍是 neutral(全 0)，应随时间推进到目标。
    // 注意：Tick 的单帧 dt 上限为 0.1s（防卡顿跳变），故用多步推进。
    d.Tick(0.0f);
    Check(Nearly(d.Weights()[0], 0.0f, 1e-3f), "过渡起点 happy 权重≈0");
    for (int i = 0; i < 12; ++i) d.Tick(0.05f);  // 累计 0.6s > 0.3s 过渡时长
    Check(Nearly(d.Weights()[0], 0.85f, 1e-3f), "过渡结束 happy 权重≈0.85");

    Check(d.ApplyPreset("sad"), "ApplyPreset(sad) 成功");
    for (int i = 0; i < 12; ++i) d.Tick(0.05f);
    Check(Nearly(d.Weights()[1], 0.80f, 1e-3f), "切到 sad 后权重≈0.80");
    Check(Nearly(d.Weights()[0], 0.0f, 1e-3f), "happy 通道回落到 0");
}

void TestUnavailablePreset() {
    std::printf("-- 预设不可用 --\n");
    // 通道里没有 happy 相关名字，预设应拒绝。
    ExpressionDirector d;
    d.Bind({"a", "b", "c"});
    Check(!d.ApplyPreset("happy"), "happy 在只有 a/b/c 的模型上不可用");
    Check(d.ApplyPreset("neutral"), "neutral 永远可用");
}

void TestAutoBlinkOverlay() {
    std::printf("-- 自动眨眼独立叠层 --\n");
    ExpressionDirector d;
    d.Bind(SampleChannels());
    d.ApplyPreset("happy");
    d.Tick(1.0f);  // 稳定到 happy
    // 强制制造一次眨眼：关闭自动后再开，等足够长时间触发。
    d.SetAutoBlink(false);
    Check(!d.AutoBlink(), "可关闭自动眨眼");
    d.SetAutoBlink(true);
    // 多帧推进直到 blink 波形出现（用 sin 包络，最高到 1）。
    bool sawBlink = false;
    for (int i = 0; i < 600 && !sawBlink; ++i) {
        d.Tick(0.016f);
        if (d.Weights()[3] > 0.5f) sawBlink = true;
    }
    Check(sawBlink, "一段时间后出现 blink 叠层");
    // blink 不影响 happy 的预设权重（独立叠层用 max）。
    Check(d.Weights()[0] > 0.5f, "blink 不破坏 happy 预设权重");
}

void TestMotionOverlayAndRelease() {
    std::printf("-- 动作表情临时叠层与淡出 --\n");
    ExpressionDirector d;
    d.Bind(SampleChannels());
    d.ApplyPreset("neutral");
    d.Tick(1.0f);
    d.SetMotionWeights({{"happy", 1.0f}}, 0.8f);
    d.Tick(0.0f);
    Check(d.Weights()[0] > 0.7f, "动作叠层后 happy 权重升高");
    d.ClearMotionWeights();
    for (int i = 0; i < 8; ++i) d.Tick(0.1f);  // 累计 0.8s，足以完成 0.2s 淡出
    Check(Nearly(d.Weights()[0], 0.0f, 1e-2f), "淡出后回落到 neutral");
}

void TestBinaryQuantize() {
    std::printf("-- 二元表情量化 --\n");
    // blink 标记为二元：半值应量化成 0/1。
    std::vector<char> binary(11, 0);
    binary[3] = 1;  // blink 二元
    ExpressionDirector d;
    d.Bind(SampleChannels(), &binary);
    d.ApplyPreset("neutral");
    d.Tick(1.0f);
    // 直接给 blink 半值（绕过预设，用 SetWeight）。
    Check(d.SetWeight("blink", 0.4f), "SetWeight(blink,0.4) 成功");
    d.Tick(0.0f);
    Check(Nearly(d.Weights()[3], 0.0f, 1e-3f), "二元表情 0.4 -> 量化 0");
    d.SetWeight("blink", 0.7f);
    d.Tick(0.0f);
    Check(Nearly(d.Weights()[3], 1.0f, 1e-3f), "二元表情 0.7 -> 量化 1");
}

void TestTimeline() {
    std::printf("-- 表情时间轴 ExpressionTimeline --\n");
    ExpressionTimeline tl;
    Check(tl.Record(0.0f, 2.0f, {{"happy", 0.0f}, {"sad", 1.0f}}), "记录首帧");
    Check(tl.Record(1.0f, 2.0f, {{"happy", 1.0f}, {"sad", 0.0f}}), "记录中帧");
    Check(tl.Record(2.0f, 2.0f, {{"happy", 0.0f}, {"sad", 1.0f}}), "记录尾帧");
    Check(tl.FrameCount() == 3, "共 3 帧");
    auto mid = tl.Sample(1.0f, ClipEasing::Linear);
    Check(mid.size() == 2, "中帧采样 2 个通道");
    // 时间越界：clamp 到首/尾帧。
    Check(tl.Sample(-1.0f, ClipEasing::Linear).front().second == 0.0f, "负时间 clamp 到首帧");
    Check(tl.Sample(99.0f, ClipEasing::Linear).front().second == 0.0f, "超时 clamp 到尾帧");
    // 同时间覆盖而非新增。
    Check(tl.Record(1.0f, 2.0f, {{"happy", 0.5f}}), "同时间覆盖");
    Check(tl.FrameCount() == 3, "覆盖后仍为 3 帧");
    tl.Remove(1.0f);
    Check(tl.FrameCount() == 2, "删除一帧后剩 2 帧");
    Check(!tl.Record(99.0f, 2.0f, {{"happy", 1.0f}}), "越界时间拒绝");
}

}  // namespace

int main() {
    std::printf("=== ExpressionDirector 单元测试 ===\n");
    TestAliasResolve();
    TestPresetApplyAndTransition();
    TestUnavailablePreset();
    TestAutoBlinkOverlay();
    TestMotionOverlayAndRelease();
    TestBinaryQuantize();
    TestTimeline();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
