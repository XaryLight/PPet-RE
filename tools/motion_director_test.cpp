// PPet - C++ 移植版
// Live2D 动作调度（MotionDirector.hpp）单元测试。
//
// 背景：旧 Live2D 后端把 model3.json 的所有动作组混进一个数组随机播放，
// 组名丢失、互动动作会在待机时自己冒出来、点击也没有反应。本次参考
// 原 ppet / Touhou Desktop Pet / Cubism Framework 的动作优先级模型重写：
//   Idle 组随机待机循环 + HitArea 分区点击触发互动 + 优先级打断 + 淡入淡出。
//
// MotionDirector 是纯逻辑、零 GL/Cubism 依赖，可脱离 SDK 单测，覆盖：
// 动作组分类、命中区名解析、空间分区、待机循环、互动触发与优先级、
// fade 权重、动作结束回落、退化输入。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <api/live2d/MotionDirector.hpp>

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

void TestClassify() {
    std::printf("-- 动作组分类 ClassifyMotionGroup --\n");
    Check(ClassifyMotionGroup("Idle") == MotionGroupKind::Idle, "Idle -> Idle");
    Check(ClassifyMotionGroup("idle_01") == MotionGroupKind::Idle, "idle_01 -> Idle");
    Check(ClassifyMotionGroup("IDLE") == MotionGroupKind::Idle, "IDLE -> Idle(大小写)");
    Check(ClassifyMotionGroup("TapHead") == MotionGroupKind::Tap, "TapHead -> Tap");
    Check(ClassifyMotionGroup("TapBody") == MotionGroupKind::Tap, "TapBody -> Tap");
    Check(ClassifyMotionGroup("PinchIn") == MotionGroupKind::Pinch, "PinchIn -> Pinch");
    Check(ClassifyMotionGroup("Shake") == MotionGroupKind::Shake, "Shake -> Shake");
    Check(ClassifyMotionGroup("FlickHead") == MotionGroupKind::Flick, "FlickHead -> Flick");
    Check(ClassifyMotionGroup("ExpHappy") == MotionGroupKind::Expression,
          "ExpHappy -> Expression");
    Check(ClassifyMotionGroup("") == MotionGroupKind::Other, "空名 -> Other");
    Check(ClassifyMotionGroup("Dance") == MotionGroupKind::Other, "Dance -> Other");
    Check(ClassifyMotionGroup("Tap@Head") == MotionGroupKind::Tap, "Tap@Head -> Tap");
}

void TestHitAreaName() {
    std::printf("-- 命中区名解析 HitAreaFromGroupName --\n");
    auto kind = MotionGroupKind::Tap;
    Check(HitAreaFromGroupName("TapHead", kind) == "head", "TapHead -> head");
    Check(HitAreaFromGroupName("TapBody", kind) == "body", "TapBody -> body");
    Check(HitAreaFromGroupName("PinchLeg", MotionGroupKind::Pinch) == "leg",
          "PinchLeg -> leg");
    Check(HitAreaFromGroupName("tap_head", kind) == "head", "tap_head -> head(下划线)");
    Check(HitAreaFromGroupName("Tap", kind).empty(), "Tap(无部位) -> 空");
    Check(HitAreaFromGroupName("Idle", MotionGroupKind::Idle).empty(),
          "Idle -> 空(非互动组)");
    Check(HitAreaFromGroupName("Flick-Hand", MotionGroupKind::Flick) == "hand",
          "Flick-Hand -> hand(连字符)");
}

void TestRegion() {
    std::printf("-- 空间分区 HitRegionFromPoint --\n");
    Check(HitRegionFromPoint(0.5f, 0.1f) == "head", "上方中部 -> head");
    Check(HitRegionFromPoint(0.5f, 0.5f) == "body", "正中 -> body");
    Check(HitRegionFromPoint(0.5f, 0.9f) == "leg", "下方中部 -> leg");
    Check(HitRegionFromPoint(0.1f, 0.5f) == "left", "左侧 -> left");
    Check(HitRegionFromPoint(0.9f, 0.5f) == "right", "右侧 -> right");
    Check(HitRegionFromPoint(0.1f, 0.1f) == "left", "左上角 -> left(横向优先)");
    Check(HitRegionFromPoint(0.0f, 1.0f) == "left", "边界坐标不越界 -> left");
}

// 构造一个典型模型：Idle×2 + TapHead×1 + TapBody×1 + Exp×1，命中区 head/body。
MotionDirector MakeTypical() {
    std::vector<MotionGroup> groups;
    MotionGroup idle;
    idle.name = "Idle";
    idle.kind = MotionGroupKind::Idle;
    idle.motionIndices = {0, 1};
    groups.push_back(idle);
    MotionGroup tapHead;
    tapHead.name = "TapHead";
    tapHead.kind = MotionGroupKind::Tap;
    tapHead.hitArea = "head";
    tapHead.motionIndices = {2};
    groups.push_back(tapHead);
    MotionGroup tapBody;
    tapBody.name = "TapBody";
    tapBody.kind = MotionGroupKind::Tap;
    tapBody.hitArea = "body";
    tapBody.motionIndices = {3};
    groups.push_back(tapBody);
    MotionGroup exp;
    exp.name = "ExpHappy";
    exp.kind = MotionGroupKind::Expression;
    exp.motionIndices = {4};
    groups.push_back(exp);

    std::vector<std::string> hitAreas = {"head", "body"};
    MotionDirector d;
    d.Configure(std::move(groups), std::move(hitAreas), 5);
    // 时长 / 淡入淡出（与 motions 下标一一对应）
    std::vector<float> dur = {3.0f, 3.0f, 2.0f, 2.0f, 1.5f};
    std::vector<float> fin = {0.5f, 0.5f, 0.2f, 0.2f, 0.0f};
    std::vector<float> fout = {0.5f, 0.5f, 0.3f, 0.3f, 0.0f};
    d.SetMotionMeta(dur, fin, fout);
    d.SetIdleCooldown(1.0f, 0.0f);  // 固定冷却，便于断言时序
    return d;
}

void TestIdleLoop() {
    std::printf("-- 待机循环 Idle --\n");
    MotionDirector d = MakeTypical();
    Check(d.HasIdleGroups(), "存在 Idle 组");
    Check(d.HasInteractionGroups(), "存在互动组");
    Check(d.GroupCount() == 4, "动作组数=4");
    Check(d.HitAreaCount() == 2, "命中区数=2");

    // 首次 Tick 立即起一个 Idle 动作（Configure 不预选，元数据已就位）。
    d.Tick(0.0f);
    Check(d.Current().motionIndex >= 0, "首帧起 Idle 动作");
    Check(d.Current().priority == MotionPriority::Idle, "Idle 动作优先级=Idle");
    const int first = d.Current().motionIndex;
    Check(first == 0 || first == 1, "首动作来自 Idle 组(下标 0/1)");

    // 推进到时长结束：应进入冷却（current 清空），冷却后再起下一个 Idle。
    d.Tick(3.1f);
    Check(d.Current().motionIndex < 0, "Idle 播放结束 -> 进入冷却(无动作)");
    d.Tick(1.1f);  // 冷却 1 秒后
    Check(d.Current().motionIndex >= 0, "冷却结束 -> 重新起 Idle 动作");
    Check(d.Current().priority == MotionPriority::Idle, "回落的仍是 Idle");
}

void TestInteraction() {
    std::printf("-- 点击互动 Interact --\n");
    {
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);  // 起 Idle
        Check(d.Current().priority == MotionPriority::Idle, "互动前处于 Idle");
        // 点击头部区域 (0.5, 0.1) -> 应命中 TapHead
        Check(d.Interact(0.5f, 0.1f), "点击头部触发互动成功");
        Check(d.Current().priority >= MotionPriority::Normal,
              "互动动作优先级 >= Normal");
        Check(d.Current().groupName == "TapHead", "命中 TapHead 组");
        Check(d.Current().motionIndex == 2, "播放 TapHead 的动作(下标 2)");
    }
    {
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);
        Check(d.Interact(0.5f, 0.5f), "点击身体触发互动成功");
        Check(d.Current().groupName == "TapBody", "命中 TapBody 组");
    }
    {
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);
        // 点击左侧 (0.1, 0.5) -> 无对应互动组，回退到任意互动组（不报错）
        Check(d.Interact(0.1f, 0.5f), "无匹配命中区时回退到任意互动组");
        Check(d.Current().priority >= MotionPriority::Normal, "回退仍是互动动作");
    }
}

void TestPriority() {
    std::printf("-- 优先级打断 --\n");
    {
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);
        Check(d.Interact(0.5f, 0.1f), "第一次互动成功");
        Check(d.Current().groupName == "TapHead", "当前 TapHead");
        // 普通优先级：正在播放互动时不打断（time < 0.15s 合并重复点击）
        Check(!d.Interact(0.5f, 0.5f, MotionPriority::Normal),
              "普通互动不能打断进行中的互动");
        Check(d.Current().groupName == "TapHead", "未被打断仍是 TapHead");
        // 强制优先级：可打断
        Check(d.Interact(0.5f, 0.5f, MotionPriority::Force),
              "Force 互动可打断进行中的互动");
        Check(d.Current().groupName == "TapBody", "被 Force 打断为 TapBody");
    }
    {
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);  // Idle
        Check(d.Interact(0.5f, 0.1f, MotionPriority::Force),
              "Force 可打断 Idle");
    }
}

void TestInteractionFallbackToIdle() {
    std::printf("-- 互动结束回落待机 --\n");
    MotionDirector d = MakeTypical();
    d.Tick(0.0f);
    Check(d.Interact(0.5f, 0.1f), "触发 TapHead");
    Check(d.Current().groupName == "TapHead", "当前 TapHead");
    // TapHead 时长 2.0，推进到结束
    d.Tick(2.1f);
    Check(d.Current().motionIndex < 0 || d.Current().priority == MotionPriority::Idle,
          "互动结束 -> 不再处于互动态");
    // 再推进越过互动后的短冷却(0.2) + idle 冷却(1.0)
    d.Tick(1.5f);
    Check(d.Current().priority == MotionPriority::Idle, "最终回落到 Idle 待机");
}

void TestFade() {
    std::printf("-- 淡入淡出权重 FadeWeight --\n");
    MotionDirector d = MakeTypical();
    d.Tick(0.0f);
    // Idle 动作 fadeIn=0.5：起步权重应为 0，逐渐升到 1。
    Check(Nearly(d.Blend().weight, 0.0f), "t=0 淡入权重=0");
    d.Tick(0.25f);
    Check(Nearly(d.Blend().weight, 0.5f, 1e-3f), "t=0.25 淡入权重≈0.5");
    d.Tick(0.25f);
    Check(Nearly(d.Blend().weight, 1.0f, 1e-3f), "t=0.5 淡入完成权重=1");
    // fadeOut=0.5，时长 3.0：t=2.75 时剩余 0.25，权重≈0.5
    d.Tick(2.25f);
    Check(Nearly(d.Blend().weight, 0.5f, 1e-3f), "接近结尾淡出权重≈0.5");
}

void TestDegenerate() {
    std::printf("-- 退化输入 --\n");
    {
        MotionDirector d;
        d.Configure({}, {}, 0);
        d.Tick(1.0f);
        Check(d.Blend().motionIndex < 0, "空配置无动作");
        Check(!d.Interact(0.5f, 0.5f), "空配置互动返回 false");
        Check(!d.HasIdleGroups() && !d.HasInteractionGroups(), "空配置无组");
    }
    {
        // 只有 Idle 组、没有互动组：点击应返回 false（不会误触发待机当互动）
        MotionDirector d;
        std::vector<MotionGroup> groups;
        MotionGroup idle;
        idle.name = "Idle";
        idle.kind = MotionGroupKind::Idle;
        idle.motionIndices = {0};
        groups.push_back(idle);
        d.Configure(std::move(groups), {}, 1);
        d.SetMotionMeta({2.0f}, {0.0f}, {0.0f});
        d.Tick(0.0f);
        Check(d.Current().motionIndex == 0, "纯 Idle 模型有动作");
        Check(!d.Interact(0.5f, 0.5f), "无互动组时点击返回 false");
        Check(d.Current().motionIndex == 0, "点击不改变当前 Idle 动作");
    }
    {
        // PlayGroup 按名字触发（供托盘/测试）
        MotionDirector d = MakeTypical();
        d.Tick(0.0f);
        Check(d.PlayGroup("TapBody"), "PlayGroup(TapBody) 成功");
        Check(d.Current().groupName == "TapBody", "PlayGroup 命中指定组");
        Check(!d.PlayGroup("不存在的组"), "PlayGroup 未知组返回 false");
    }
    {
        // 无 fade 时权重恒为 1
        MotionDirector d;
        std::vector<MotionGroup> groups;
        MotionGroup idle;
        idle.name = "Idle";
        idle.kind = MotionGroupKind::Idle;
        idle.motionIndices = {0};
        groups.push_back(idle);
        d.Configure(std::move(groups), {}, 1);
        d.SetMotionMeta({5.0f}, {0.0f}, {0.0f});
        d.Tick(1.0f);
        Check(Nearly(d.Blend().weight, 1.0f), "无 fade 权重恒=1");
    }
    {
        // StatusText 不空且随状态变化
        MotionDirector d = MakeTypical();
        Check(!d.StatusText().empty(), "StatusText 非空");
        d.Tick(0.0f);
        Check(!d.StatusText().empty(), "待机时 StatusText 非空");
    }
}

}  // namespace

int main() {
    std::printf("=== MotionDirector 单元测试 ===\n");
    TestClassify();
    TestHitAreaName();
    TestRegion();
    TestIdleLoop();
    TestInteraction();
    TestPriority();
    TestInteractionFallbackToIdle();
    TestFade();
    TestDegenerate();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
