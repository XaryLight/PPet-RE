// PPet - Live2D 动作分组与待机回归测试
//
// 【为什么需要这个测试】
//   本项目踩过三个很典型的坑，都表现为「桌宠看起来不动 / 不对」，
//   而且**不会崩溃、不报错**，纯靠肉眼很难定位。这里把它们固化成断言：
//
//   1) **空名动作组**（本项目实测的真实模型）：
//        "Motions": { "": [ { "File": "motions/idle.motion3.json" },
//                           { "File": "motions/touch_head.motion3.json" }, ... ] }
//      组名是空串，不含任何语义。按组名分类会得到「没有任何 Idle 组」，
//      于是待机时**一个动作都不播**——模型看上去完全静止。
//      必须能按**文件名**回退推断用途。
//
//   2) **Meta.Loop 循环标记**：
//      待机动画通常是循环的（"Meta": { "Duration": 6.0, "Loop": true }）。
//      忽略它会让待机动画播一轮就停 3~7 秒，桌宠「动一下、停很久」。
//
//   3) **动作文件名的用途推断**：
//      idle / touch_head / touch_body / exp_* 等命名是模型作者的约定，
//      分类器必须能识别它们。
//
// 测试全部是纯逻辑（不碰 Cubism SDK），三种 CMake 配置都能编译运行。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <api/live2d/MotionDirector.hpp>
#include <api/live2d/Live2DMotion.hpp>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool Nearly(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

using namespace ppet::live2d;

// ---------------------------------------------------------------------------
// 1) 按文件名推断动作用途
// ---------------------------------------------------------------------------
void TestClassifyMotionFile() {
    std::printf("-- 按文件名推断用途 ClassifyMotionFile --\n");

    Check(ClassifyMotionFile("idle.motion3.json") == MotionGroupKind::Idle,
          "idle.motion3.json -> Idle");
    Check(ClassifyMotionFile("motions/idle.motion3.json") == MotionGroupKind::Idle,
          "motions/idle.motion3.json -> Idle（带目录）");
    Check(ClassifyMotionFile("Idle.motion3.json") == MotionGroupKind::Idle,
          "Idle.motion3.json -> Idle（大小写不敏感）");
    Check(ClassifyMotionFile("stand.motion3.json") == MotionGroupKind::Idle,
          "stand.motion3.json -> Idle");
    Check(ClassifyMotionFile("wait.motion3.json") == MotionGroupKind::Idle,
          "wait.motion3.json -> Idle");

    Check(ClassifyMotionFile("touch_head.motion3.json") == MotionGroupKind::Tap,
          "touch_head.motion3.json -> Tap");
    Check(ClassifyMotionFile("tap_body.motion3.json") == MotionGroupKind::Tap,
          "tap_body.motion3.json -> Tap");
    Check(ClassifyMotionFile("touch_special.motion3.json") == MotionGroupKind::Tap,
          "touch_special.motion3.json -> Tap");
    Check(ClassifyMotionFile("pinch.motion3.json") == MotionGroupKind::Pinch,
          "pinch.motion3.json -> Pinch");
    Check(ClassifyMotionFile("shake.motion3.json") == MotionGroupKind::Shake,
          "shake.motion3.json -> Shake");
    Check(ClassifyMotionFile("flick.motion3.json") == MotionGroupKind::Flick,
          "flick.motion3.json -> Flick");
    Check(ClassifyMotionFile("exp_happy.motion3.json") == MotionGroupKind::Expression,
          "exp_happy.motion3.json -> Expression");
    Check(ClassifyMotionFile("emotion.motion3.json") == MotionGroupKind::Expression,
          "emotion.motion3.json -> Expression");

    // 互动类型优先于 idle：tap_idle 应当算互动而不是待机。
    Check(ClassifyMotionFile("tap_idle.motion3.json") == MotionGroupKind::Tap,
          "tap_idle.motion3.json -> Tap（互动优先于待机）");

    // 无法判断时返回 Other（不是 Idle）。
    Check(ClassifyMotionFile("mail.motion3.json") == MotionGroupKind::Other,
          "mail.motion3.json -> Other");
    Check(ClassifyMotionFile("main_1.motion3.json") == MotionGroupKind::Other,
          "main_1.motion3.json -> Other");
}

// ---------------------------------------------------------------------------
// 2) 从文件名提取命中区
// ---------------------------------------------------------------------------
void TestHitAreaFromMotionFile() {
    std::printf("-- 从文件名提取命中区 HitAreaFromMotionFile --\n");

    Check(HitAreaFromMotionFile("touch_head.motion3.json") == "head",
          "touch_head -> head");
    Check(HitAreaFromMotionFile("motions/touch_body.motion3.json") == "body",
          "motions/touch_body -> body");
    Check(HitAreaFromMotionFile("tap_hand.motion3.json") == "hand",
          "tap_hand -> hand");
    Check(HitAreaFromMotionFile("idle.motion3.json").empty(),
          "idle -> 空（没有命中区）");
}

// ---------------------------------------------------------------------------
// 3) 空名组必须能产生 Idle 组
// ---------------------------------------------------------------------------
void TestEmptyNamedGroupStillHasIdle() {
    std::printf("-- 空名动作组仍能识别待机组 --\n");

    // 模拟本项目实测的真实模型：所有动作都在空名组下。
    std::vector<MotionGroup> groups;
    auto add = [&](const std::string& file, MotionGroupKind kind,
                   const std::string& hitArea) {
        MotionGroup g;
        g.name = file;
        g.kind = kind;
        g.hitArea = hitArea;
        g.motionIndices.push_back(static_cast<int>(groups.size()));
        groups.push_back(std::move(g));
    };
    add("idle.motion3.json", ClassifyMotionFile("idle.motion3.json"),
        HitAreaFromMotionFile("idle.motion3.json"));
    add("touch_head.motion3.json", ClassifyMotionFile("touch_head.motion3.json"),
        HitAreaFromMotionFile("touch_head.motion3.json"));
    add("touch_body.motion3.json", ClassifyMotionFile("touch_body.motion3.json"),
        HitAreaFromMotionFile("touch_body.motion3.json"));

    MotionDirector director;
    director.Configure(groups, {"head", "body"}, 3);

    Check(director.HasIdleGroups(), "空名组场景下存在待机组");
    Check(director.HasInteractionGroups(), "空名组场景下存在互动组");

    // 待机应当真的能起一个动作（而不是「无动作」）。
    director.Tick(0.016f);
    const MotionBlend blend = director.Blend();
    Check(blend.motionIndex >= 0, "待机确实起了动作");
    Check(blend.weight > 0.0f, "待机动作权重 > 0");
    Check(director.Current().groupName == "idle.motion3.json", "待机组被选中");

    // 点击头部应当命中 touch_head。
    bool interacted = false;
    for (int i = 0; i < 20 && !interacted; ++i) {
        interacted = director.Interact(0.5f, 0.1f);
    }
    Check(interacted, "点击头部有互动响应");
    Check(director.Current().groupName == "touch_head.motion3.json",
          "点击头部命中 touch_head");
}

// ---------------------------------------------------------------------------
// 4) Meta.Loop 循环标记
// ---------------------------------------------------------------------------
void TestLoopMeta() {
    std::printf("-- 循环标记 Loop --\n");

    // 这里直接验证 MotionDirector 的循环行为（不依赖文件 IO）。
    std::vector<MotionGroup> groups;
    MotionGroup g;
    g.name = "idle.motion3.json";
    g.kind = MotionGroupKind::Idle;
    g.motionIndices.push_back(0);
    groups.push_back(std::move(g));

    MotionDirector director;
    director.Configure(groups, {}, 1);
    director.SetMotionMeta({6.0f}, {0.0f}, {0.0f}, {true});

    director.Tick(0.016f);
    Check(director.Current().loop, "循环标记被传入");

    // 播放 7 秒（超过 6 秒时长）：循环的动作应当还在播，
    // 而非播完就停（那样桌宠会「动一下、停很久」）。
    for (int i = 0; i < 420; ++i) director.Tick(1.0f / 60.0f);
    Check(director.Current().motionIndex >= 0, "循环动作 7 秒后仍在播放");
    Check(director.Current().time < 6.0f, "循环动作的时间回绕到一轮之内");

    // 对照组：不带循环标记时，播完一轮会进入冷却（不再有动作）。
    MotionDirector plain;
    plain.Configure(groups, {}, 1);
    plain.SetMotionMeta({6.0f}, {0.0f}, {0.0f}, {false});
    plain.Tick(0.016f);
    for (int i = 0; i < 420; ++i) plain.Tick(1.0f / 60.0f);
    Check(plain.Current().motionIndex < 0, "非循环动作播完一轮后进入冷却");
}

// ---------------------------------------------------------------------------
// 5) Segments 解析（Cubism 5 扁平数组）
// ---------------------------------------------------------------------------
void TestSegmentsParsing() {
    std::printf("-- Segments 解析（Cubism 5）--\n");

    // 用临时文件验证完整解析链路。
    const char* kPath = "/tmp/ppet_motion_segments_test.motion3.json";
    const char* kJson = R"({
      "Version": 3,
      "Meta": { "Duration": 6.0, "Fps": 60.0, "Loop": true, "CurveCount": 2 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamAngleX",
          "Segments": [ 0.0, -20.0, 0, 3.0, -8.0, 0, 6.0, -20.0 ] },
        { "Target": "Parameter", "Id": "ParamBreath",
          "Segments": [ 0.0, 0.0, 1, 1.0, 5.0, 2.0, 10.0, 6.0, 0.0 ] }
      ]
    })";
    std::FILE* fp = std::fopen(kPath, "wb");
    if (!fp) {
        Check(false, "无法写入临时动作文件");
        return;
    }
    std::fwrite(kJson, 1, std::strlen(kJson), fp);
    std::fclose(fp);

    Motion motion;
    std::string error;
    const bool ok = ParseMotionFile(kPath, motion, error);
    Check(ok, "Segments 格式动作解析成功");
    if (!ok) {
        std::printf("        错误：%s\n", error.c_str());
        std::remove(kPath);
        return;
    }
    Check(Nearly(motion.duration, 6.0f), "时长解析正确");
    Check(motion.loop, "Meta.Loop 解析正确");
    Check(motion.curves.size() == 2, "两条曲线都被解析");

    // 线性曲线：起点 -20，3 秒处 -8，6 秒处 -20。
    if (motion.curves.size() >= 1) {
        const MotionCurve& c = motion.curves[0];
        Check(c.id == "ParamAngleX", "第一条曲线是 ParamAngleX");
        Check(c.keys.size() == 3, "线性曲线解析出 3 个关键帧");
        if (c.keys.size() == 3) {
            Check(Nearly(c.keys[0].time, 0.0f) && Nearly(c.keys[0].value, -20.0f),
                  "关键帧 0 = (0s, -20)");
            Check(Nearly(c.keys[1].time, 3.0f) && Nearly(c.keys[1].value, -8.0f),
                  "关键帧 1 = (3s, -8)");
            Check(Nearly(c.keys[2].time, 6.0f) && Nearly(c.keys[2].value, -20.0f),
                  "关键帧 2 = (6s, -20)");
            Check(c.keys[0].outType == CurveType::Linear, "段标识 0 -> 线性");
        }
    }

    // 贝塞尔曲线：段标识 1。
    //
    // 【官方格式】官方规范原文：
    //   "A segment identifier is followed by *1 point* in case of linear,
    //    stepped, and inverse stepped segments, that represents the end of
    //    the segment, or *3 points* in case of bézier segments, that
    //    represent P1, P2, P3."
    // 下面这串数字解析为：
    //   点0 = (0.0s, 0.0)
    //   段标识 = 1（贝塞尔）
    //   P1 = (1.0s, 5.0)，P2 = (2.0s, 10.0)，P3 = (6.0s, 0.0)
    // 即两个关键帧 + 一段贝塞尔过渡（P3 是终点）。
    if (motion.curves.size() >= 2) {
        const MotionCurve& c = motion.curves[1];
        Check(c.keys.size() == 2, "贝塞尔曲线解析出 2 个关键帧");
        if (c.keys.size() == 2) {
            Check(c.keys[0].outType == CurveType::Bezier, "段标识 1 -> 贝塞尔");
            Check(c.keys[1].inType == CurveType::Bezier, "下一关键帧的入段类型为贝塞尔");
            Check(Nearly(c.keys[0].time, 0.0f) && Nearly(c.keys[0].value, 0.0f),
                  "贝塞尔关键帧 0 = (0s, 0)");
            Check(Nearly(c.keys[1].time, 6.0f) && Nearly(c.keys[1].value, 0.0f),
                  "贝塞尔关键帧 1 = (6s, 0)");
        }
    }

    // 采样：起点应为 -20，终点应为 -20，中点应接近 -8（线性）。
    std::vector<float> values;
    motion.Sample(0.0f, values);
    Check(values.size() == 2 && Nearly(values[0], -20.0f), "采样 t=0 得到 -20");
    motion.Sample(3.0f, values);
    Check(values.size() == 2 && Nearly(values[0], -8.0f), "采样 t=3 得到 -8");
    motion.Sample(6.0f, values);
    Check(values.size() == 2 && Nearly(values[0], -20.0f), "采样 t=6 得到 -20");

    std::remove(kPath);
}

// ---------------------------------------------------------------------------
// 6) 旧版关键帧数组（Cubism 3/4）仍然可用
// ---------------------------------------------------------------------------
void TestLegacyCurveParsing() {
    std::printf("-- 旧版 Curve 关键帧数组（Cubism 3/4）--\n");

    const char* kPath = "/tmp/ppet_motion_legacy_test.motion3.json";
    const char* kJson = R"({
      "Version": 3,
      "Meta": { "Duration": 2.0, "Fps": 30.0, "CurveCount": 1 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamAngleY",
          "Curve": [
            { "Time": 0.0, "Value": 0.0, "In": "linear", "Out": "linear" },
            { "Time": 2.0, "Value": 10.0, "In": "linear", "Out": "linear" }
          ] }
      ]
    })";
    std::FILE* fp = std::fopen(kPath, "wb");
    if (!fp) {
        Check(false, "无法写入临时动作文件");
        return;
    }
    std::fwrite(kJson, 1, std::strlen(kJson), fp);
    std::fclose(fp);

    Motion motion;
    std::string error;
    const bool ok = ParseMotionFile(kPath, motion, error);
    Check(ok, "旧版格式解析成功");
    if (ok) {
        Check(motion.curves.size() == 1, "一条曲线");
        std::vector<float> values;
        motion.Sample(1.0f, values);
        Check(values.size() == 1 && Nearly(values[0], 5.0f), "线性插值中点为 5");
    }
    std::remove(kPath);
}

}  // namespace

int main() {
    std::printf("=== Live2D 动作分组与待机回归测试 ===\n");
    TestClassifyMotionFile();
    TestHitAreaFromMotionFile();
    TestEmptyNamedGroupStillHasIdle();
    TestLoopMeta();
    TestSegmentsParsing();
    TestLegacyCurveParsing();

    if (g_failures == 0) {
        std::printf("=== 全部通过 ===\n");
        return 0;
    }
    std::printf("=== 失败 %d 项 ===\n", g_failures);
    return 1;
}
