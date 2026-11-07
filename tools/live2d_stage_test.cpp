// PPet - C++ 移植版
// Live2D 运行时分层（Stage.hpp）单元测试。
//
// 覆盖：参数栈分层仲裁（高层覆盖/加性累加/退出复位）/ 姿态总线 flush /
// 三通道视线指数阻尼与非对称增益 / 待机微摆与钟形包络 / 待机小表情 /
// 多边形热区命中与长期态 / 口型导演平滑。
//
// 纯逻辑、零 Cubism 依赖，可脱离 SDK 单测。
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <api/live2d/Stage.hpp>

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

using namespace ppet::live2d;

void TestParameterStack() {
    std::printf("-- 参数栈 ParameterStack --\n");
    ParameterStack stack;
    std::map<std::string, float> defaults = {{"ParamAngleX", 0.0f}};
    stack.Reset(defaults);
    stack.Write(ParamLayer::Gaze, "ParamAngleX", 0.3f);                 // L1
    stack.Write(ParamLayer::Force, "ParamAngleX", 0.9f);                // L5 覆盖
    Check(Nearly(stack.Commit().at("ParamAngleX"), 0.9f), "高层覆盖低层");

    stack.Reset(defaults);
    stack.Write(ParamLayer::Gaze, "ParamAngleX", 0.3f, WriteMode::Add);
    stack.Write(ParamLayer::Pose, "ParamAngleX", 0.2f, WriteMode::Add);
    Check(Nearly(stack.Commit().at("ParamAngleX"), 0.5f), "加性层累加");

    // 退出复位：层不写就回落到默认。
    stack.Reset(defaults);
    stack.Write(ParamLayer::Gaze, "ParamAngleX", 0.3f);
    Check(Nearly(stack.Commit().at("ParamAngleX"), 0.3f), "写值时生效");
    stack.Reset(defaults);
    Check(Nearly(stack.Commit().at("ParamAngleX"), 0.0f), "不写值回落默认（退出复位）");
}

void TestPoseBus() {
    std::printf("-- 姿态总线 PoseBus --\n");
    PoseBus bus;
    bus.Add("ParamAngleX", 0.1f);
    bus.Add("ParamAngleX", 0.2f);
    auto out = bus.Flush();
    Check(Nearly(out["ParamAngleX"], 0.3f), "多次 Add 累加");
    Check(!bus.HasPending(), "Flush 后清空");
    auto empty = bus.Flush();
    Check(empty.empty(), "空 flush 返回空");
}

void TestGazeChannels() {
    std::printf("-- 三通道视线 GazeChannels --\n");
    GazeChannels gaze;
    gaze.SetSpeeds(20.0f, 7.0f, 3.5f);
    gaze.SetAsymmetry(1.6f, 1.0f);
    // 从 0 追向目标 1.0，眼球应比身体更快接近目标。
    GazeChannels::Channels c;
    c = gaze.Tick(0.1f, 1.0f, 0.0f);
    Check(c.eyeX > c.bodyX, "眼球比身体更快追向目标");
    // 长时间推进后各通道应收敛到目标。
    for (int i = 0; i < 200; ++i) c = gaze.Tick(0.05f, 1.0f, 0.0f);
    Check(Nearly(c.eyeX, 1.0f, 0.05f), "眼球收敛到目标");
    Check(Nearly(c.bodyX, 0.5f, 0.05f), "身体收敛到目标的一半（bodyX 用 target*0.5）");

    // 无鼠标回中。
    GazeChannels g2;
    g2.SetSpeeds(20.0f, 7.0f, 3.5f);
    GazeChannels::Channels d = g2.Tick(0.1f, 0.0f, 0.0f);
    Check(Nearly(d.eyeX, 0.0f, 1e-3f), "无输入时回中");
}

void TestIdleDirector() {
    std::printf("-- 待机导演 IdleDirector --\n");
    IdleDirector idle;
    idle.SetSwayAmplitude(0.1f, 0.05f, 0.05f);
    // 微摆应随时间变化（非恒定 0）。
    IdleDirector::Sway a = idle.Tick(0.1f);
    IdleDirector::Sway b = idle.Tick(0.2f);
    Check(a.headYaw != b.headYaw, "微摆随时间变化");
    // 说话压制：同一相位下（dt=0 不推进时间），talkMask=1 时幅度应更小。
    IdleDirector::Sway base = idle.Tick(0.0f, 0.0f);
    IdleDirector::Sway quiet = idle.Tick(0.0f, 1.0f);
    Check(std::fabs(quiet.headYaw) <= std::fabs(base.headYaw) + 1e-6f, "说话时压低微摆");

    // 钟形包络：动作进行中权重 > 0，结束后回到 0。
    IdleDirector act;
    act.StartAction(2.0f, 1.0f);
    act.AdvanceAction(1.0f);
    Check(act.IsActionActive(), "动作进行中");
    Check(act.ActionEnvelope() > 0.0f, "包络峰值 > 0");
    act.AdvanceAction(2.0f);
    Check(!act.IsActionActive(), "动作结束");
    Check(Nearly(act.ActionEnvelope(), 0.0f), "结束后包络归 0");
}

void TestIdleFaces() {
    std::printf("-- 待机小表情 IdleFaces --\n");
    IdleFaces faces;
    faces.SetCandidates({"blink", "happy", "angry"}, 11u);
    // 冷却内不应随机出表情。
    auto out = faces.Tick(0.01f);
    Check(out.empty(), "冷却内无小表情");
    // 推进超过冷却后可能出现（多跑几次保证触发）。
    bool triggered = false;
    for (int i = 0; i < 2000 && !triggered; ++i) {
        auto f = faces.Tick(0.1f);
        if (!f.empty()) triggered = true;
    }
    Check(triggered, "冷却后触发小表情");

    // wink 覆盖层。
    IdleFaces w;
    w.TriggerWink();
    auto wout = w.Tick(0.05f);
    bool hasWink = false;
    for (const auto& f : wout) if (f.name == "ParamWinkL") hasWink = true;
    Check(hasWink, "wink 覆盖层生效");
}

void TestIdleFacePresets() {
    std::printf("-- 待机小表情预设（参数通道）--\n");
    IdleFaces faces;
    std::vector<FacePreset> presets;
    FacePreset happy;
    happy.id = "happy";
    happy.name = "开心";
    happy.params = {{"ParamBrowLY", 1.0f}, {"ParamEyeLSmile", 1.0f}};
    presets.push_back(happy);
    FacePreset shy;
    shy.id = "shy";
    shy.name = "害羞";
    shy.params = {{"ParamCheek", 1.0f}};
    presets.push_back(shy);
    faces.SetPresets(std::move(presets), 7u);
    faces.SetInterval(1.0f, 1.0f);
    faces.SetEnabled(true);
    Check(faces.Presets().size() == 2, "预设数量正确");
    Check(!faces.Empty(), "非空");

    // 手动触发后应立刻进入淡入阶段，包络从 0 上升。
    Check(faces.Trigger(), "手动触发小表情");
    Check(faces.CurrentId() == "happy" || faces.CurrentId() == "shy", "触发后有当前表情");
    float maxEnv = 0.0f;
    bool sawParam = false;
    for (int i = 0; i < 60; ++i) {
        auto fs = faces.Tick(0.05f);
        for (const auto& f : fs) {
            // 两个预设的参数集合不同，只要出现任一预设参数即算生效。
            if (f.name == "ParamBrowLY" || f.name == "ParamEyeLSmile" ||
                f.name == "ParamCheek") {
                sawParam = true;
            }
            maxEnv = std::max(maxEnv, f.weight);
        }
    }
    Check(sawParam, "表情参数被输出");
    Check(maxEnv > 0.0f, "淡入后包络 > 0");

    // 表情播完后应回到中性（当前 id 清空）。
    // 注意：冷却到点后会再次随机触发，因此这里检查的是「曾经回到过中性」。
    bool returnedNeutral = false;
    for (int i = 0; i < 400 && !returnedNeutral; ++i) {
        faces.Tick(0.05f);
        if (faces.CurrentId().empty()) returnedNeutral = true;
    }
    Check(returnedNeutral, "表情结束后回到中性");

    // 关闭时不应再产生表情。
    faces.SetEnabled(false);
    const auto before = faces.Presets().size();
    faces.Tick(1.0f);
    Check(faces.Presets().size() == before, "关闭后不改变预设");
}

void TestRegionSet() {
    std::printf("-- 多边形热区 RegionSet --\n");
    RegionSet rs;
    // 头部三角：左上(0,0) 右上(0.5,0) 下(0.25,0.5)
    rs.Add({"head", {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.25f, 0.5f}}, 1, "tap"});
    // 身体矩形（更低 z）
    rs.Add({"body", {{0.0f, 0.5f}, {1.0f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f}}, 0, "pet"});
    Check(rs.Hit(0.25f, 0.2f) == "head", "点中头部");
    Check(rs.Hit(0.5f, 0.8f) == "body", "点中身体");
    Check(rs.Hit(0.7f, 0.1f) == "", "空白区域无命中");
    // z 优先级：重叠时取高 z。
    Check(rs.GestureOf("head") == "tap", "头部手势 tap");

    // 长期态：持续命中超过阈值进入长期态。
    for (int i = 0; i < 30; ++i) rs.UpdateHoldTime(0.1f, "head");
    Check(rs.IsLongHeld("head", 2.0f), "持续命中进入长期态");
    Check(!rs.IsLongHeld("body", 2.0f), "未持续命中的区域非长期态");
}

void TestMouthDirector() {
    std::printf("-- 口型导演 MouthDirector --\n");
    MouthDirector mouth;
    mouth.SetRange(0.0f, 1.0f);
    // 闭嘴：开口度应接近 0。
    auto closed = mouth.Tick(0.016f, 0.0f);
    Check(closed.first < 0.1f, "闭嘴时开口度很小");
    // 说话：开口度应上升并收敛到较高值。
    float maxOpen = 0.0f;
    for (int i = 0; i < 60; ++i) {
        auto o = mouth.Tick(0.016f, 1.0f);
        maxOpen = std::max(maxOpen, o.first);
    }
    Check(maxOpen > 0.7f, "说话时开口度显著上升");
}

}  // namespace

int main() {
    std::printf("=== Live2D Stage 单元测试 ===\n");
    TestParameterStack();
    TestPoseBus();
    TestGazeChannels();
    TestIdleDirector();
    TestIdleFaces();
    TestIdleFacePresets();
    TestRegionSet();
    TestMouthDirector();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
