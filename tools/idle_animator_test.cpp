// PPet - C++ 移植版
// 程序化待机动画（IdleAnimator）单元测试。
//
// 覆盖：
//   1. 基础待机：各通道确实有幅度（早期版本只有 1.5° 摆动，几乎看不出来）
//   2. 整体幅度倍率：masterAmount = 0 时完全静止
//   3. 频率互质：呼吸 / 重心 / 头部 / 手臂 四个通道的周期互不相同
//   4. 小动作：延迟触发、钟形包络（起止归零、中间有峰值）、播完自动回到待机
//   5. 随机可复现：同一种子跑两遍，姿态序列完全一致
//   6. dt 钳制：超大 dt 不会让姿态瞬移
#include <src/tools/model/IdleAnimator.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

/// 统计一条通道在给定帧数里的最大绝对值与极差。
struct ChannelStats {
    float maxAbs = 0.0f;
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

ChannelStats StatsOf(const std::vector<ppet::model::IdlePose>& poses,
                     float (*pick)(const ppet::model::IdlePose&)) {
    ChannelStats stats;
    stats.minValue = pick(poses.front());
    stats.maxValue = stats.minValue;
    for (const auto& pose : poses) {
        const float v = pick(pose);
        stats.maxAbs = std::max(stats.maxAbs, std::fabs(v));
        stats.minValue = std::min(stats.minValue, v);
        stats.maxValue = std::max(stats.maxValue, v);
    }
    return stats;
}

void TestBasicChannels() {
    ppet::model::IdleAnimator anim(1234u);

    std::vector<ppet::model::IdlePose> poses;
    poses.reserve(600);
    for (int i = 0; i < 600; ++i) {
        poses.push_back(anim.Tick(1.0f / 60.0f));
    }

    const auto headYaw = StatsOf(poses, [](const ppet::model::IdlePose& p) { return p.headYaw; });
    const auto armPitch =
        StatsOf(poses, [](const ppet::model::IdlePose& p) { return p.leftArmPitch; });
    const auto chest =
        StatsOf(poses, [](const ppet::model::IdlePose& p) { return p.chestScale; });
    const auto sway =
        StatsOf(poses, [](const ppet::model::IdlePose& p) { return p.weightShift; });

    // 头部微摆至少要能看到（0.05 rad ≈ 2.9°）。
    Check(headYaw.maxAbs > 0.05f, "头部微摆幅度应明显可见");
    Check(armPitch.maxAbs > 0.03f, "手臂摆动幅度应明显可见");
    Check(chest.maxAbs > 0.005f, "呼吸应驱动胸腔缩放");
    Check(sway.maxAbs > 0.5f, "重心转移应接近满量程");

    // 重心左右摆：既有正也有负。
    Check(sway.minValue < -0.2f && sway.maxValue > 0.2f, "重心应左右往复而非单向偏移");

    // 抬腿高度应由重心方向唯一决定：同一时刻只可能有一条腿抬起。
    const ppet::model::IdlePose last = poses.back();
    Check(last.leftLegLift * last.rightLegLift <= 1e-9f,
          "同一时刻只应有一条腿抬起（重心偏移唯一决定抬腿）");
}

void TestMasterAmountZero() {
    ppet::model::IdleAnimator anim(99u);
    ppet::model::IdleAnimator::Config config = anim.GetConfig();
    config.masterAmount = 0.0f;
    anim.SetConfig(config);
    anim.Reset();

    bool allZero = true;
    for (int i = 0; i < 120; ++i) {
        const auto pose = anim.Tick(1.0f / 60.0f);
        if (std::fabs(pose.headYaw) > 1e-6f || std::fabs(pose.chestScale) > 1e-6f ||
            std::fabs(pose.weightShift) > 1e-6f || std::fabs(pose.leftArmPitch) > 1e-6f) {
            allZero = false;
            break;
        }
    }
    Check(allZero, "masterAmount = 0 时不应有任何待机动作");
}

void TestFrequenciesAreDistinct() {
    // 四个通道的频率必须互不相同，否则会出现肉眼可见的循环节拍。
    const ppet::model::IdleAnimator::Config config;
    Check(config.breathSpeed != config.swaySpeed, "呼吸与重心频率不应相同");
    Check(config.swaySpeed != config.headSpeed, "重心与头部频率不应相同");
    Check(config.headSpeed != config.armSpeed, "头部与手臂频率不应相同");
    Check(config.breathSpeed != config.headSpeed, "呼吸与头部频率不应相同");
    Check(config.breathSpeed != config.armSpeed, "呼吸与手臂频率不应相同");
    Check(config.swaySpeed != config.armSpeed, "重心与手臂频率不应相同");
}

void TestGestureLifecycle() {
    ppet::model::IdleAnimator anim(4242u);

    // 刚复位时不应立刻触发小动作。
    anim.Tick(1.0f / 60.0f);
    Check(!anim.InGesture(), "复位后不应立即触发小动作");
    Check(anim.TimeToNextGesture() > 0.0f, "复位后应有剩余等待时间");

    // 手动触发，验证钟形包络：起点为 0、中间有峰值、终点回到 0。
    anim.TriggerGesture(ppet::model::IdleAnimator::Gesture::LookAround);
    Check(anim.InGesture(), "手动触发后应处于小动作状态");
    Check(anim.CurrentGesture() == ppet::model::IdleAnimator::Gesture::LookAround,
          "小动作类型应与触发时一致");

    // 包络起点为 0：触发后立刻取一帧，与触发前的姿态几乎无差别。
    const auto before = anim.Tick(1.0f / 60.0f);
    anim.TriggerGesture(ppet::model::IdleAnimator::Gesture::LookAround);
    const auto atStart = anim.Tick(1e-4f);
    Check(std::fabs(atStart.headYaw - before.headYaw) < 0.01f,
          "小动作刚触发时包络应接近 0");

    // 推到中间，包络应达到峰值（头部大幅转动）。
    ppet::model::IdleAnimator::Config config = anim.GetConfig();
    config.gestureDuration = 2.0f;
    anim.SetConfig(config);
    anim.TriggerGesture(ppet::model::IdleAnimator::Gesture::LookAround);
    anim.Tick(0.001f);

    float peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        const auto p = anim.Tick(1.0f / 60.0f);
        peak = std::max(peak, std::fabs(p.headYaw));
    }
    Check(peak > 0.2f, "小动作中段应有明显的头部转动");

    // 播完自动回到待机。
    for (int i = 0; i < 240; ++i) {
        anim.Tick(1.0f / 60.0f);
    }
    Check(!anim.InGesture(), "小动作播完应自动回到待机");
    Check(anim.TimeToNextGesture() > 0.0f, "小动作结束后应重新开始计时");
}

void TestRandomGestureTriggers() {
    ppet::model::IdleAnimator anim(777u);
    ppet::model::IdleAnimator::Config config = anim.GetConfig();
    config.gestureMinInterval = 1.0f;
    config.gestureMaxInterval = 2.0f;
    anim.SetConfig(config);
    anim.Reset();

    // 间隔设为 1~2 秒，跑 30 秒应该触发多次小动作。
    int triggers = 0;
    bool wasInGesture = false;
    for (int i = 0; i < 1800; ++i) {
        anim.Tick(1.0f / 60.0f);
        if (anim.InGesture() && !wasInGesture) {
            ++triggers;
        }
        wasInGesture = anim.InGesture();
    }
    Check(triggers >= 3, "随机小动作应按间隔自动触发");
}

void TestReproducible() {
    ppet::model::IdleAnimator a(2024u);
    ppet::model::IdleAnimator b(2024u);
    for (int i = 0; i < 300; ++i) {
        const auto pa = a.Tick(1.0f / 60.0f);
        const auto pb = b.Tick(1.0f / 60.0f);
        if (std::fabs(pa.headYaw - pb.headYaw) > 1e-6f) {
            Check(false, "同一种子应产生完全一致的姿态序列");
            return;
        }
    }
    Check(true, "同一种子应产生完全一致的姿态序列");
}

void TestLargeDtIsClamped() {
    ppet::model::IdleAnimator a(5u);
    ppet::model::IdleAnimator b(5u);

    // a: 一次推进 10 秒（dt 被钳到 0.1 秒）
    const auto pa = a.Tick(10.0f);
    // b: 一次推进 0.1 秒（同样推进 0.1 秒）
    const auto pb = b.Tick(0.1f);
    Check(std::fabs(pa.headYaw - pb.headYaw) < 1e-4f, "超大 dt 应被钳制到 0.1 秒");
}

} // namespace

int main() {
    TestBasicChannels();
    TestMasterAmountZero();
    TestFrequenciesAreDistinct();
    TestGestureLifecycle();
    TestRandomGestureTriggers();
    TestReproducible();
    TestLargeDtIsClamped();

    if (g_failures == 0) {
        std::printf("[OK] IdleAnimator 全部用例通过\n");
        return 0;
    }
    std::printf("[FAIL] IdleAnimator %d 项断言失败\n", g_failures);
    return 1;
}
