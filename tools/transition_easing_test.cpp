// PPet - C++ 移植版
// 动画过渡原语单测（报告：动画播放间自然过渡）：
//   1. 两条曲线的端点值 / 单调性 / 中点值
//   2. crossfade 权重守恒：满权重切换时 w_out + w_in === 1
//   3. 二次打断（fromWeight<1）时权重总和不超过 1，不会凭空放大
//   4. 骨骼播放器交叉淡化：合成姿态幅度不超过单个动作（消除过冲）
#include <src/tools/core/TransitionEasing.hpp>
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
        std::printf("[FAIL] %s（实际 %.5f，期望 %.5f ±%.5f）\n", message, actual, expected,
                    tolerance);
        ++g_failures;
    }
}

using ppet::transition::Curve;

void TestEasingCurves() {
    // 端点：两条曲线都是 Ease(0)=0, Ease(1)=1。
    CheckNear(ppet::transition::Ease(0.0f, Curve::SmoothStep), 0.0f, 1e-6f,
              "SmoothStep: Ease(0)=0");
    CheckNear(ppet::transition::Ease(1.0f, Curve::SmoothStep), 1.0f, 1e-6f,
              "SmoothStep: Ease(1)=1");
    CheckNear(ppet::transition::Ease(0.5f, Curve::SmoothStep), 0.5f, 1e-6f,
              "SmoothStep: Ease(0.5)=0.5（对称）");
    CheckNear(ppet::transition::Ease(0.0f, Curve::Sine), 0.0f, 1e-6f, "Sine: Ease(0)=0");
    CheckNear(ppet::transition::Ease(1.0f, Curve::Sine), 1.0f, 1e-6f, "Sine: Ease(1)=1");
    CheckNear(ppet::transition::Ease(0.5f, Curve::Sine), 0.5f, 1e-5f,
              "Sine: Ease(0.5)=0.5（对称）");

    // 单调性：0→1 全程不下降。
    for (Curve curve : {Curve::SmoothStep, Curve::Sine}) {
        float prev = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            const float s = i / 100.0f;
            const float v = ppet::transition::Ease(s, curve);
            Check(v >= prev - 1e-6f, "缓动曲线应单调不减");
            Check(v >= 0.0f && v <= 1.0f, "缓动曲线输出应在 [0,1]");
            prev = v;
        }
    }

    // 钳制：越界输入被夹到 [0,1]。
    CheckNear(ppet::transition::Ease(-2.0f), 0.0f, 1e-6f, "Ease 钳制下界");
    CheckNear(ppet::transition::Ease(5.0f), 1.0f, 1e-6f, "Ease 钳制上界");
}

void TestCrossFadeConservation() {
    // 满权重切换（from=to=1）：任意进度下两层权重和恒为 1。
    for (Curve curve : {Curve::SmoothStep, Curve::Sine}) {
        for (int i = 0; i <= 20; ++i) {
            const float p = i / 20.0f;
            const auto w = ppet::transition::CrossFade(1.0f, 1.0f, p, curve);
            CheckNear(w.outgoing + w.incoming, 1.0f, 1e-6f,
                      "满权重 crossfade 权重和应为 1");
        }
    }

    // 二次打断（from=0.4）：权重和从 0.4 单调升到 1，中间不超过 1（不过冲）。
    float prevSum = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float p = i / 20.0f;
        const auto w = ppet::transition::CrossFade(0.4f, 1.0f, p);
        const float sum = w.outgoing + w.incoming;
        Check(sum <= 1.0f + 1e-6f, "二次打断权重和不应超过 1");
        Check(sum >= prevSum - 1e-6f, "二次打断权重和应单调不减");
        prevSum = sum;
    }
}

ppet::model::BoneMotion ConstantMotion(const std::string& bone, float pitch) {
    ppet::model::BoneMotion motion;
    motion.id = "constant";
    motion.duration = 5.0f;
    motion.loop = false;
    motion.keys = {{bone, 0.0f, pitch, 0.0f, 0.0f, 1.0f},
                   {bone, 5.0f, pitch, 0.0f, 0.0f, 1.0f}};
    return motion;
}

float PitchOf(const ppet::model::BonePose& pose, const std::string& bone) {
    for (const auto& entry : pose.entries) {
        if (entry.bone == bone) return entry.pitch;
    }
    return 0.0f;
}

void TestPlayerNoOvershoot() {
    using namespace ppet::model;
    // 从 +30° 恒定姿态切到 -30°：若两层权重和 =1，合成姿态应始终落在 [-30,30]。
    // 旧实现淡入 0.1s / 淡出 0.28s 权重和>1 会放大到 ~±40°（过冲）。
    BoneMotionPlayer player;
    player.SetCrossFade(0.28f);
    player.SetEndFade(0.0f);
    player.Play(ConstantMotion("head", 30.0f), 0.0f);

    BonePose pose;
    player.Tick(0.1f, pose);
    CheckNear(PitchOf(pose, "head"), 30.0f, 0.5f, "切换前应处于 +30°");

    player.Play(ConstantMotion("head", -30.0f), 0.1f);
    float worstOvershoot = 0.0f;
    for (int i = 0; i < 40; ++i) {
        player.Tick(1.0f / 60.0f, pose);
        const float pitch = PitchOf(pose, "head");
        worstOvershoot = std::max(worstOvershoot, std::fabs(pitch) - 30.0f);
    }
    Check(worstOvershoot < 0.6f, "交叉淡化合成姿态不应超过 ±30°（无过冲）");

    // 切到 Sine 曲线后同样守恒。
    BoneMotionPlayer sinePlayer;
    sinePlayer.SetCrossFade(0.28f);
    sinePlayer.SetEndFade(0.0f);
    sinePlayer.SetCrossFadeCurve(ppet::transition::Curve::Sine);
    Check(sinePlayer.CrossFadeCurve() == ppet::transition::Curve::Sine,
          "曲线配置应可读写");
    sinePlayer.Play(ConstantMotion("head", 30.0f), 0.0f);
    sinePlayer.Tick(0.1f, pose);
    sinePlayer.Play(ConstantMotion("head", -30.0f), 0.1f);
    float sineOvershoot = 0.0f;
    for (int i = 0; i < 40; ++i) {
        sinePlayer.Tick(1.0f / 60.0f, pose);
        sineOvershoot = std::max(sineOvershoot, std::fabs(PitchOf(pose, "head")) - 30.0f);
    }
    Check(sineOvershoot < 0.6f, "Sine 曲线交叉淡化同样不应过冲");
}

}  // namespace

int main() {
    TestEasingCurves();
    TestCrossFadeConservation();
    TestPlayerNoOvershoot();
    if (g_failures == 0) {
        std::printf("transition_easing_test：全部通过\n");
        return 0;
    }
    std::printf("transition_easing_test：%d 项失败\n", g_failures);
    return 1;
}
