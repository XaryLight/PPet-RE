// PPet - C++ 移植版
// 轻量行为状态机（BehaviorStateMachine）单元测试。
//
// 覆盖：
//   1. 初始状态 / 待机幅度 / 触摸许可
//   2. 触摸 → Reacting 与余韵超时回落
//   3. 连续触摸刷新余韵（「被摸头后的余韵」）
//   4. 动作播完提前回落
//   5. 挂起为最高优先级，拒绝触摸且不被超时打断
//   6. 拖拽交互 / 打断反应
//   7. 窗口坐姿预留状态
//   8. 驻留时间、Reset 与非法 dt
#include <src/tools/model/BehaviorStateMachine.hpp>

#include <cmath>
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
using ppet::model::BehaviorStateMachine;

void TestInitialState() {
    BehaviorStateMachine sm;
    Check(sm.State() == BehaviorState::Idle, "初始状态应为 Idle");
    Check(sm.IsIdle(), "IsIdle 应为 true");
    Check(sm.CanAcceptTouch(), "Idle 应接受触摸");
    Check(std::fabs(sm.IdleMasterScale() - 1.0f) < 1e-6f, "Idle 待机倍率应为 1.0");
}

void TestTouchAndTimeout() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    Check(sm.State() == BehaviorState::Reacting, "触摸后应进入 Reacting");
    Check(sm.IsReacting(), "IsReacting 应为 true");
    Check(std::fabs(sm.ReactionTimeLeft() - 1.6f) < 1e-5f, "余韵计时初始应为 1.6s");
    Check(std::fabs(sm.IdleMasterScale() - 0.45f) < 1e-6f, "Reaction 待机倍率应为 0.45");

    sm.Tick(1.0f);
    Check(sm.State() == BehaviorState::Reacting, "1 秒后仍应在 Reacting");
    sm.Tick(0.7f);
    Check(sm.State() == BehaviorState::Idle, "余韵耗尽后应回到 Idle");
    Check(std::fabs(sm.ReactionTimeLeft()) < 1e-6f, "回到 Idle 后余韵清零");
}

void TestReentryRefreshesHold() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    sm.Tick(1.4f);
    Check(sm.ReactionTimeLeft() < 0.3f, "接近超时时余韵应小于 0.3s");
    sm.OnTouch();
    Check(std::fabs(sm.ReactionTimeLeft() - 1.6f) < 1e-5f, "连续触摸应刷新余韵");
    sm.Tick(0.5f);
    Check(sm.State() == BehaviorState::Reacting, "刷新后不应提前回 Idle");
}

void TestReactionFinished() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    sm.OnReactionFinished();
    Check(sm.State() == BehaviorState::Idle, "动作播完应回 Idle");
    sm.OnReactionFinished();
    Check(sm.State() == BehaviorState::Idle, "Idle 下调用不应有副作用");
}

void TestSuspend() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    sm.OnSuspend();
    Check(sm.IsSuspended(), "应进入 Suspended");
    Check(!sm.CanAcceptTouch(), "挂起时应拒绝触摸");
    Check(std::fabs(sm.IdleMasterScale() - 0.25f) < 1e-6f, "挂起待机倍率应为 0.25");

    sm.OnTouch();
    Check(sm.IsSuspended(), "挂起时触摸不应改变状态");
    sm.Tick(10.0f);
    Check(sm.IsSuspended(), "挂起不应被余韵超时打断");

    sm.OnResume();
    Check(sm.IsIdle(), "恢复后应回 Idle");
    Check(sm.CanAcceptTouch(), "恢复后应接受触摸");
}

void TestInteraction() {
    BehaviorStateMachine sm;
    sm.OnInteractionBegin();
    Check(sm.IsInteracting(), "应进入 Interacting");
    Check(std::fabs(sm.IdleMasterScale() - 0.7f) < 1e-6f, "交互倍率应为 0.7");
    sm.OnInteractionEnd();
    Check(sm.IsIdle(), "结束交互应回 Idle");
}

void TestInteractionOverridesReacting() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    sm.OnInteractionBegin();
    Check(sm.IsInteracting(), "拖拽应打断反应");
    sm.OnTouch();
    Check(sm.IsReacting(), "交互中触摸应进入 Reacting");
    sm.OnInteractionEnd();
    Check(sm.IsReacting(), "非交互状态下 OnInteractionEnd 不应改状态");
}

void TestSitStand() {
    BehaviorStateMachine sm;
    sm.OnSit();
    Check(sm.State() == BehaviorState::Sitting, "应进入 Sitting");
    Check(std::fabs(sm.IdleMasterScale() - 0.85f) < 1e-6f, "坐姿倍率应为 0.85");
    sm.OnStand();
    Check(sm.IsIdle(), "站起应回 Idle");
}

void TestTimeAndReset() {
    BehaviorStateMachine sm;
    sm.Tick(0.5f);
    Check(std::fabs(sm.TimeInState() - 0.5f) < 1e-5f, "驻留时间应累加");
    sm.OnTouch();
    Check(std::fabs(sm.TimeInState()) < 1e-6f, "转换后驻留时间应清零");
    sm.Tick(0.3f);
    sm.Reset();
    Check(sm.IsIdle(), "Reset 应回 Idle");
    Check(std::fabs(sm.TimeInState()) < 1e-6f, "Reset 应清驻留时间");
    Check(std::fabs(sm.ReactionTimeLeft()) < 1e-6f, "Reset 应清余韵");
}

void TestInvalidDt() {
    BehaviorStateMachine sm;
    sm.OnTouch();
    sm.Tick(-1.0f);
    Check(std::fabs(sm.ReactionTimeLeft() - 1.6f) < 1e-5f, "负 dt 不应推进");
    sm.Tick(0.0f);
    Check(std::fabs(sm.ReactionTimeLeft() - 1.6f) < 1e-5f, "零 dt 不应推进");
}

}  // namespace

int main() {
    TestInitialState();
    TestTouchAndTimeout();
    TestReentryRefreshesHold();
    TestReactionFinished();
    TestSuspend();
    TestInteraction();
    TestInteractionOverridesReacting();
    TestSitStand();
    TestTimeAndReset();
    TestInvalidDt();

    if (g_failures == 0) {
        std::printf("[OK] BehaviorStateMachine 全部用例通过\n");
        return 0;
    }
    std::printf("[FAIL] BehaviorStateMachine %d 项断言失败\n", g_failures);
    return 1;
}
