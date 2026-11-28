// PPet - C++ 移植版
// 凝视跟随单测：验证综合 dsh-plugin-vrmpet3d / STVrmPet 的凝视自然化契约——
//   1) 目标指数平滑（快速甩鼠标不瞬移）
//   2) 均匀 ramp 起停（凝视可见地渐入渐出）
//   3) 鼠标静止休眠与恢复
//   4) epsilon 抗抖动（只影响休眠计时，不限制慢速移动）
//   5) 头部预算让位（动作占用头部时凝视收缩）
//   6) NaN/Inf 有限值防御
#include <src/tools/model/GazeFollow.hpp>

#include <cmath>
#include <cstdio>

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

bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

using ppet::model::GazeFollow;

GazeFollow::Output Advance(GazeFollow& f, int frames, float dt = 1.0f / 60.0f) {
    GazeFollow::Output out;
    for (int i = 0; i < frames; ++i) out = f.Tick(dt);
    return out;
}

/// 造一个不受休眠/预算干扰、只测平滑的对象。
GazeFollow Plain() {
    GazeFollow f;
    f.SetIdleSeconds(0.0f);
    f.SetHeadBudget(0.0f);
    f.SetWeight(1.0f);
    return f;
}

void TestAimSmoothing() {
    std::printf("-- 目标指数平滑（不瞬移）--\n");
    GazeFollow f = Plain();
    f.SetBlendSeconds(0.0f);   // 关闭 ramp，纯看平滑
    f.SetAim(0.3f, 0.0f);
    const GazeFollow::Output first = f.Tick(1.0f / 60.0f);
    Check(first.yaw > 0.0f && first.yaw < 0.1f, "首帧 yaw 远小于目标（渐入）");

    const GazeFollow::Output full = Advance(f, 120);  // 2 秒充分收敛
    Check(Near(full.yaw, 0.3f), "2 秒后 yaw 收敛到目标 0.3");

    // 快速甩到反向：单帧不应瞬移到新目标。
    f.SetAim(-0.3f, 0.0f);
    const GazeFollow::Output flip = f.Tick(1.0f / 60.0f);
    Check(flip.yaw > 0.0f, "甩鼠标单帧仍在原侧（未瞬移）");
    const GazeFollow::Output reversed = Advance(f, 120);
    Check(Near(reversed.yaw, -0.3f), "2 秒后收敛到反向目标 -0.3");
}

void TestRampInOut() {
    std::printf("-- 均匀 ramp 起停 --\n");
    GazeFollow f = Plain();
    f.SetBlendSeconds(0.5f);
    f.SetAim(0.3f, 0.0f);
    Advance(f, 120);  // 先让平滑收敛

    const GazeFollow::Output settled = f.Tick(1.0f / 60.0f);
    Check(Near(settled.blend, 1.0f), "目标稳定后 blend=1");

    f.ClearAim();
    const GazeFollow::Output outFirst = f.Tick(1.0f / 60.0f);
    Check(outFirst.blend > 0.8f && outFirst.blend < 1.0f, "淡出首帧 blend 仍接近 1（不瞬断）");
    Check(outFirst.yaw > 0.0f, "淡出沿最后方向（yaw 仍为正）");
    const GazeFollow::Output outFull = Advance(f, 60);
    Check(Near(outFull.blend, 0.0f), "淡出完成 blend=0");
    Check(Near(outFull.yaw, 0.0f), "淡出完成 yaw=0");
}

void TestIdleSleep() {
    std::printf("-- 鼠标静止休眠与恢复 --\n");
    GazeFollow f;
    f.SetBlendSeconds(0.1f);
    f.SetIdleSeconds(1.0f);
    f.SetWeight(1.0f);
    f.SetHeadBudget(0.0f);

    f.SetAim(0.2f, 0.1f);
    Advance(f, 30);  // 0.5s
    Check(f.Blend() > 0.9f, "静止未满 1s 时仍在凝视");

    // 完全相同的目标（帧间变化 < epsilon）：不刷新静止计时。
    for (int i = 0; i < 45; ++i) {
        f.SetAim(0.2f, 0.1f);
        f.Tick(1.0f / 60.0f);
    }
    Check(f.Blend() < 0.5f, "静止超过 1s 后 blend 下降（休眠）");

    // 鼠标真正移动：恢复凝视。
    f.SetAim(-0.2f, 0.0f);
    Advance(f, 30);
    Check(f.Blend() > 0.9f, "鼠标移动后恢复凝视");
    const GazeFollow::Output resumed = f.Tick(1.0f / 60.0f);
    Check(resumed.yaw < 0.0f, "恢复后朝向新目标（yaw 变负）");
}

void TestEpsilonAllowsSlowMove() {
    std::printf("-- epsilon 只挡休眠计时、不冻结目标 --\n");
    GazeFollow f = Plain();
    f.SetBlendSeconds(0.0f);
    f.SetMoveEpsilon(0.02f);

    // epsilon 只决定是否刷新「静止计时」，绝不限制目标本身。
    // 关闭休眠后，即使每帧移动量都小于 epsilon，目标也必须持续更新。
    for (int i = 0; i < 60; ++i) {
        f.SetAim(0.005f * static_cast<float>(i), 0.0f);
        f.Tick(1.0f / 60.0f);
    }
    const GazeFollow::Output out = f.Tick(1.0f / 60.0f);
    Check(out.yaw > 0.25f, "小于 epsilon 的慢速移动仍被跟随（目标不被冻结）");
}

void TestHeadBudget() {
    std::printf("-- 头部预算让位 --\n");
    GazeFollow f;
    f.SetBlendSeconds(0.1f);
    f.SetIdleSeconds(0.0f);
    f.SetWeight(1.0f);
    f.SetYawLimit(1.0f);       // 57°
    f.SetHeadBudget(0.7f);     // 40°

    f.SetAim(1.0f, 0.0f);      // 目标顶到 yawLimit
    Advance(f, 120);
    const GazeFollow::Output noMotion = f.Tick(1.0f / 60.0f);
    Check(Near(noMotion.yaw, 0.7f), "无动作时凝视受 headBudget 上限（0.7）");

    // 动作占用 0.5 rad：剩余预算 0.7 - 0.5 = 0.2。
    f.SetMotionHeadTurn(0.5f);
    const GazeFollow::Output withMotion = f.Tick(1.0f / 60.0f);
    Check(withMotion.yaw <= 0.2f + 1e-3f && withMotion.yaw > 0.0f,
          "动作占用后凝视收缩到剩余预算（<=0.2）");

    // 动作占用超过预算：凝视应为 0（完全让位），不出现反方向过冲。
    f.SetMotionHeadTurn(1.0f);
    const GazeFollow::Output fully = f.Tick(1.0f / 60.0f);
    Check(Near(fully.yaw, 0.0f), "动作超过预算时凝视完全让位");
    Check(fully.yaw >= 0.0f, "预算让位不产生反方向过冲");
}

void TestFiniteGuards() {
    std::printf("-- NaN / Inf 有限值防御 --\n");
    GazeFollow f;
    f.SetBlendSeconds(std::nanf(""));
    f.SetIdleSeconds(std::nanf(""));
    f.SetWeight(std::nanf(""));
    f.SetYawLimit(std::nanf(""));
    f.SetPitchLimits(std::nanf(""), std::nanf(""));
    f.SetHeadBudget(std::nanf(""));
    f.SetMoveEpsilon(std::nanf(""));
    f.SetFollowSmoothing(std::nanf(""));

    // NaN 目标应被忽略。
    f.SetAim(std::nanf(""), std::nanf(""));
    Check(!f.HasAim(), "NaN 目标被忽略");

    f.SetAim(0.2f, 0.1f);
    const GazeFollow::Output out = Advance(f, 60);
    Check(std::isfinite(out.yaw) && std::isfinite(out.pitch) && std::isfinite(out.blend),
          "正常目标下输出全部有限");

    const GazeFollow::Output nanDt = f.Tick(std::nanf(""));
    Check(std::isfinite(nanDt.yaw) && std::isfinite(nanDt.blend), "NaN dt 下输出仍有限");

    // 巨大的 dt（后台恢复）被钳制，blend 不会一帧跳到 1。
    GazeFollow f2;
    f2.SetBlendSeconds(1.0f);
    f2.SetIdleSeconds(0.0f);
    f2.SetAim(0.5f, 0.0f);
    const GazeFollow::Output big = f2.Tick(10.0f);
    Check(big.blend <= 0.2f, "巨大 dt 被钳制，blend 不跳变");
}

void TestReset() {
    std::printf("-- Reset 复位 --\n");
    GazeFollow f = Plain();
    f.SetBlendSeconds(0.1f);
    f.SetAim(0.5f, 0.2f);
    Advance(f, 20);
    Check(f.Blend() > 0.5f && f.HasAim(), "复位前状态生效");
    f.Reset();
    Check(!f.HasAim() && Near(f.Blend(), 0.0f), "复位后无目标且 blend=0");
    const GazeFollow::Output out = f.Tick(1.0f / 60.0f);
    Check(Near(out.yaw, 0.0f) && Near(out.pitch, 0.0f), "复位后输出为 0");
}

}  // namespace

int main() {
    std::printf("=== GazeFollow 凝视跟随单测 ===\n");
    TestAimSmoothing();
    TestRampInOut();
    TestIdleSleep();
    TestEpsilonAllowsSlowMove();
    TestHeadBudget();
    TestFiniteGuards();
    TestReset();
    std::printf("结果：%d/%d 通过，%d 失败\n", checks - failures, checks, failures);
    return failures == 0 ? 0 : 1;
}
