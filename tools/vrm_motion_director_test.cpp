// PPet - C++ 移植版
// VRM 动作调度器（VrmMotionDirector）单元测试。
//
// 覆盖：
//   1. 空调度器：所有状态回退程序化
//   2. 注册 Idle 源：Idle 状态选外部，Reacting 状态回退程序化
//   3. 注册 Touch 源：Reacting 状态选外部
//   4. 同类优先级仲裁
//   5. 便捷查询（UseExternalIdle / UseExternalTouch）
//   6. HasCategory / ClearSources / SourceCount
#include <src/tools/model/vrm/VrmMotionDirector.hpp>

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

using ppet::model::BehaviorState;
using ppet::vrm::VrmMotionCategory;
using ppet::vrm::VrmMotionDirector;
using ppet::vrm::VrmMotionSource;

void TestEmptyFallsBack() {
    VrmMotionDirector d;
    auto decision = d.Select(BehaviorState::Idle);
    Check(!decision.useExternal, "空调度器 Idle 不应选外部");
    Check(decision.fallbackProcedural, "空调度器 Idle 应回退程序化");
    Check(decision.category == VrmMotionCategory::Idle, "Idle 状态类别应为 Idle");

    decision = d.Select(BehaviorState::Reacting);
    Check(!decision.useExternal, "空调度器 Reacting 不应选外部");
    Check(decision.fallbackProcedural, "空调度器 Reacting 应回退程序化");
    Check(decision.category == VrmMotionCategory::Touch, "Reacting 状态类别应为 Touch");
}

void TestIdleSource() {
    VrmMotionDirector d;
    d.RegisterSource({VrmMotionCategory::Idle, 1, "gltf", true});

    auto decision = d.Select(BehaviorState::Idle);
    Check(decision.useExternal, "注册 Idle 源后 Idle 状态应选外部");
    Check(decision.sourceIndex == 0, "应选中第 0 个源");
    Check(decision.category == VrmMotionCategory::Idle, "类别应为 Idle");

    decision = d.Select(BehaviorState::Sitting);
    Check(decision.useExternal, "Sitting 状态也应选外部 Idle 源");

    decision = d.Select(BehaviorState::Reacting);
    Check(!decision.useExternal, "Reacting 状态不应选 Idle 源");
    Check(decision.fallbackProcedural, "Reacting 状态应回退程序化（内置骨骼动作）");
}

void TestTouchSource() {
    VrmMotionDirector d;
    d.RegisterSource({VrmMotionCategory::Touch, 1, "vrma:tap", false});

    auto decision = d.Select(BehaviorState::Reacting);
    Check(decision.useExternal, "注册 Touch 源后 Reacting 应选外部");
    Check(decision.category == VrmMotionCategory::Touch, "类别应为 Touch");

    decision = d.Select(BehaviorState::Interacting);
    Check(decision.useExternal, "Interacting 也应选外部 Touch 源");

    decision = d.Select(BehaviorState::Idle);
    Check(!decision.useExternal, "Idle 状态不应选 Touch 源");
}

void TestPriority() {
    VrmMotionDirector d;
    d.RegisterSource({VrmMotionCategory::Idle, 1, "gltf", true});
    d.RegisterSource({VrmMotionCategory::Idle, 5, "vrma:idle", true});

    auto decision = d.Select(BehaviorState::Idle);
    Check(decision.useExternal, "应选外部 Idle 源");
    Check(decision.sourceIndex == 1, "高优先级 VRMA 源应胜出（下标 1）");
}

void TestConvenience() {
    VrmMotionDirector d;
    Check(!d.UseExternalIdle(BehaviorState::Idle), "无源时 UseExternalIdle 应为 false");
    Check(!d.UseExternalTouch(BehaviorState::Reacting), "无源时 UseExternalTouch 应为 false");

    d.RegisterSource({VrmMotionCategory::Idle, 1, "gltf", true});
    Check(d.UseExternalIdle(BehaviorState::Idle), "注册 Idle 源后 UseExternalIdle 应为 true");
    Check(!d.UseExternalTouch(BehaviorState::Reacting), "未注册 Touch 源 UseExternalTouch 仍为 false");

    d.RegisterSource({VrmMotionCategory::Touch, 2, "vrma:tap", false});
    Check(d.UseExternalTouch(BehaviorState::Reacting), "注册 Touch 源后 UseExternalTouch 应为 true");
}

void TestBookkeeping() {
    VrmMotionDirector d;
    Check(d.SourceCount() == 0, "初始源数量应为 0");
    d.RegisterSource({VrmMotionCategory::Idle, 1, "gltf", true});
    d.RegisterSource({VrmMotionCategory::Touch, 1, "vrma:tap", false});
    Check(d.SourceCount() == 2, "注册两个源后数量应为 2");
    Check(d.HasCategory(VrmMotionCategory::Idle), "应存在 Idle 类别");
    Check(d.HasCategory(VrmMotionCategory::Touch), "应存在 Touch 类别");
    Check(!d.HasCategory(VrmMotionCategory::Emote), "不应存在 Emote 类别");

    d.ClearSources();
    Check(d.SourceCount() == 0, "ClearSources 后数量应为 0");
    Check(!d.HasCategory(VrmMotionCategory::Idle), "清空后无 Idle 类别");
}

}  // namespace

int main() {
    TestEmptyFallsBack();
    TestIdleSource();
    TestTouchSource();
    TestPriority();
    TestConvenience();
    TestBookkeeping();

    if (g_failures == 0) {
        std::printf("[OK] VrmMotionDirector 全部用例通过\n");
        return 0;
    }
    std::printf("[FAIL] VrmMotionDirector %d 项断言失败\n", g_failures);
    return 1;
}
