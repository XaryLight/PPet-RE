// PPet - C++ 移植版
// 骨骼动作（Bone Motion）单元测试。
//
// 覆盖：
//   1. 内置动作库（点头 / 摇头 / 挥手）的结构正确性
//   2. 播放器：播放 / 进度 / 淡入淡出 / 一次性动作播完自动结束
//   3. 关键帧插值：起止姿态、峰值姿态
//   4. 随机待机：间隔内不触发、超时后触发
#include <src/tools/model/BoneMotion.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void CheckNear(float actual, float expected, float tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::printf("[FAIL] %s（实际 %.3f，期望 %.3f ±%.3f）\n", message, actual, expected,
                    tolerance);
        ++g_failures;
    }
}

/// 从姿态里取某个骨骼的 pitch（不存在返回 0）。
float PitchOf(const ppet::model::BonePose& pose, const std::string& bone) {
    for (const auto& entry : pose.entries) {
        if (entry.bone == bone) return entry.pitch;
    }
    return 0.0f;
}

/// 构造一个「姿态恒定」的测试动作：整个时长内头部保持固定角度。
/// 用于验证淡入淡出这类「纯权重」行为，避免动画本身的变化干扰判断。
ppet::model::BoneMotion MakeConstantMotion(const std::string& bone, float pitch, float duration) {
    ppet::model::BoneMotion motion;
    motion.id = "constant";
    motion.name = "恒定";
    motion.duration = duration;
    motion.loop = false;
    motion.keys = {{bone, 0.0f, pitch, 0.0f, 0.0f, 1.0f},
                   {bone, duration, pitch, 0.0f, 0.0f, 1.0f}};
    return motion;
}

void TestBuiltinMotions() {
    using namespace ppet::model;
    const std::vector<BoneMotion> motions = bone_motions::Builtin();
    Check(motions.size() >= 3, "内置动作至少有 3 个");

    BoneMotion nod;
    Check(bone_motions::Find("nod", nod), "应能按 id 找到「点头」");
    Check(nod.duration > 0.0f, "点头动作时长应大于 0");
    Check(!nod.keys.empty(), "点头动作应有关键帧");

    BoneMotion shake;
    Check(bone_motions::Find("shake", shake), "应能按 id 找到「摇头」");
    Check(!shake.keys.empty(), "摇头动作应有关键帧");

    BoneMotion wave;
    Check(bone_motions::Find("wave", wave), "应能按 id 找到「挥手」");
    bool hasRightArm = false;
    for (const BoneKey& key : wave.keys) {
        if (key.bone == "rightArm") hasRightArm = true;
    }
    Check(hasRightArm, "挥手动作应驱动右臂");

    BoneMotion missing;
    Check(!bone_motions::Find("__not_exist__", missing), "不存在的 id 应返回 false");
}

void TestPlayAndProgress() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    Check(!player.IsPlaying(), "初始状态不应在播放");

    BoneMotion nod = bone_motions::Nod();
    Check(player.Play(nod, 0.0f), "播放点头动作应成功");
    Check(player.IsPlaying(), "播放后 IsPlaying 应为 true");
    Check(player.CurrentId() == "nod", "当前动作 id 应为 nod");

    BonePose pose;
    player.Tick(0.1f, pose);
    Check(player.Progress() > 0.0f, "推进后进度应大于 0");
    Check(player.Progress() < 1.0f, "推进 0.1s 后进度应小于 1");
}

void TestInterpolation() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    player.Play(bone_motions::Nod(), 0.0f);

    // 点头动作：t=0 时头是 -6 度，t=0.18 时是 +18 度（低头）。
    BonePose start;
    player.Tick(0.0f, start);
    CheckNear(PitchOf(start, "head"), -6.0f, 0.5f, "点头起始姿态头应为 -6 度");

    // 推进到峰值附近。
    BonePose peak;
    for (int i = 0; i < 3; ++i) player.Tick(0.06f, peak);
    CheckNear(PitchOf(peak, "head"), 18.0f, 1.5f, "点头峰值姿态头应为 +18 度");
}

void TestOneShotEnds() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    player.Play(bone_motions::Nod(), 0.0f);

    BonePose pose;
    // 动作总长 1.1 秒，推进 2 秒后应自动结束。
    for (int i = 0; i < 120; ++i) player.Tick(1.0f / 60.0f, pose);
    Check(!player.IsPlaying(), "一次性动作播完应自动结束");
}

void TestFadeOut() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    // 用恒定姿态动作：角度只受淡出权重影响，便于判断。
    player.Play(MakeConstantMotion("head", 20.0f, 5.0f), 0.0f);

    BonePose pose;
    player.Tick(0.1f, pose);
    const float beforeStop = PitchOf(pose, "head");
    CheckNear(beforeStop, 20.0f, 0.5f, "淡出前应处于满姿态");

    player.Stop(0.2f);
    Check(player.IsPlaying(), "淡出期间仍算在播放");

    // 淡出过程中角度应逐渐变小。
    player.Tick(0.05f, pose);
    const float midStop = PitchOf(pose, "head");
    Check(midStop < beforeStop, "淡出过程中角度应变小");
    Check(midStop > 0.0f, "淡出未结束时角度不应直接归零");

    for (int i = 0; i < 30; ++i) player.Tick(1.0f / 60.0f, pose);
    Check(!player.IsPlaying(), "淡出完成后应停止");
}

void TestFadeIn() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    // 用恒定姿态动作：角度只受淡入权重影响。
    player.Play(MakeConstantMotion("head", 20.0f, 5.0f), 0.5f);

    BonePose pose;
    player.Tick(0.05f, pose);
    const float early = PitchOf(pose, "head");
    Check(early < 20.0f, "淡入未完成时角度应被压低");
    Check(early > 0.0f, "淡入刚开始时角度不应为 0");

    // 淡入完成后角度应达到设定值。
    for (int i = 0; i < 20; ++i) player.Tick(0.05f, pose);
    CheckNear(PitchOf(pose, "head"), 20.0f, 0.5f, "淡入完成后应达到满姿态");
}

void TestRandomTrigger() {
    using namespace ppet::model;
    BoneMotionPlayer player;
    player.SetRandomRange(8.0f, 18.0f);
    player.SetRandomChance(1.0f);  // 必定触发，便于测试
    for (const BoneMotion& motion : bone_motions::Builtin()) {
        player.AddRandomMotion(motion);
    }
    Check(player.RandomMotionCount() >= 3, "随机动作池应已注册");

    // 短时间（2 秒）内不应触发（间隔最小 8 秒）。
    BonePose pose;
    for (int i = 0; i < 120; ++i) player.Tick(1.0f / 60.0f, pose);
    Check(!player.IsPlaying(), "间隔未到时不应触发随机动作");

    // 推进足够长的时间，应至少触发过一次随机动作。
    bool everPlayed = false;
    for (int i = 0; i < 60 * 60; ++i) {
        player.Tick(1.0f / 60.0f, pose);
        if (player.IsPlaying()) everPlayed = true;
    }
    Check(everPlayed, "超过随机间隔后应触发随机动作");
}

void TestWaveArmMapping() {
    using namespace ppet::model;
    BoneMotion wave = bone_motions::Wave("rightArm");
    Check(wave.id == "wave", "右臂挥手 id 应为 wave");

    BoneMotion waveLeft = bone_motions::Wave("leftArm");
    Check(waveLeft.id == "waveLeft", "左臂挥手 id 应为 waveLeft");

    // 两个动作驱动的骨骼不同。
    bool rightHasArm = false;
    bool leftHasArm = false;
    for (const BoneKey& key : wave.keys) {
        if (key.bone == "rightArm") rightHasArm = true;
    }
    for (const BoneKey& key : waveLeft.keys) {
        if (key.bone == "leftArm") leftHasArm = true;
    }
    Check(rightHasArm, "右臂挥手应驱动 rightArm");
    Check(leftHasArm, "左臂挥手应驱动 leftArm");
}

}  // namespace

int main() {
    TestBuiltinMotions();
    TestPlayAndProgress();
    TestInterpolation();
    TestOneShotEnds();
    TestFadeOut();
    TestFadeIn();
    TestRandomTrigger();
    TestWaveArmMapping();

    if (g_failures == 0) {
        std::printf("bone_motion_test：全部通过\n");
        return 0;
    }
    std::printf("bone_motion_test：%d 项失败\n", g_failures);
    return 1;
}
