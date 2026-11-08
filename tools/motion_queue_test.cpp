// PPet - C++ 移植版
// 动作队列（MotionQueue）单元测试：对齐 Cubism Native Framework 的
// MotionQueueManager / CubismMotion 语义（交叉淡入淡出 + EasingSine）。
//
// 覆盖：
//   1. EasingSine 端点与中点
//   2. 单动作淡入 / 淡出 / 无 fade
//   3. StartMotion 打断：旧动作淡出、新动作淡入，队列里同时生效
//   4. 循环动作不结束、非循环动作到时自动移除
//   5. FinishPrimary / StopAll
#include <api/live2d/MotionQueue.hpp>

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
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

using namespace ppet::live2d;

// 一个 fadeIn=0.5 / fadeOut=0.5 / duration=2.0 的动作。
QueueBlend FindBlend(const std::vector<QueueBlend>& blends, int index) {
    for (const QueueBlend& b : blends) {
        if (b.motionIndex == index) return b;
    }
    return QueueBlend();
}

void TestEasingSine() {
    std::printf("-- 官方 EasingSine --\n");
    Check(Near(EasingSine(0.0f), 0.0f), "x=0 -> 0");
    Check(Near(EasingSine(1.0f), 1.0f), "x=1 -> 1");
    Check(Near(EasingSine(0.5f), 0.5f), "x=0.5 -> 0.5");
    Check(Near(EasingSine(-3.0f), 0.0f), "负数钳到 0");
    Check(Near(EasingSine(9.0f), 1.0f), "超过 1 钳到 1");
    // 起止斜率为 0（比线性插值柔和）：0.1 处明显小于 0.1。
    Check(EasingSine(0.1f) < 0.1f, "起步斜率平缓");
}

void TestSingleMotionFade() {
    std::printf("-- 单动作淡入淡出 --\n");
    MotionQueue q;
    q.StartMotion(0, 2.0f, 0.5f, 0.5f, false, 0, "Idle", q.Clock());
    Check(q.Size() == 1, "队列里 1 条动作");
    Check(Near(FindBlend(q.Blends(), 0).weight, 0.0f), "t=0 淡入权重=0");

    q.Tick(0.25f);
    Check(Near(FindBlend(q.Blends(), 0).time, 0.25f), "时间推进到 0.25");
    // EasingSine(0.5) = 0.5
    Check(Near(FindBlend(q.Blends(), 0).weight, 0.5f, 1e-3f), "t=0.25 权重≈0.5");

    q.Tick(0.25f);
    Check(Near(FindBlend(q.Blends(), 0).weight, 1.0f, 1e-3f), "t=0.5 淡入完成=1");

    q.Tick(1.25f);  // t=1.75，距结尾 0.25，EasingSine(0.5)=0.5
    Check(Near(FindBlend(q.Blends(), 0).weight, 0.5f, 1e-3f), "t=1.75 淡出权重≈0.5");

    q.Tick(0.3f);  // t=2.05 超过时长
    Check(q.Empty(), "非循环动作播完自动出队");
}

void TestNoFade() {
    std::printf("-- 无 fade 权重恒 1 --\n");
    MotionQueue q;
    q.StartMotion(7, 5.0f, 0.0f, 0.0f, false, 0, "Idle", q.Clock());
    q.Tick(1.0f);
    Check(Near(FindBlend(q.Blends(), 7).weight, 1.0f), "无 fade 权重=1");
}

void TestCrossFade() {
    std::printf("-- 官方 StartMotion 交叉淡入淡出 --\n");
    MotionQueue q;
    q.StartMotion(0, 10.0f, 0.4f, 0.6f, false, 0, "Idle", q.Clock());
    q.Tick(1.0f);  // 旧动作完全淡入
    Check(Near(FindBlend(q.Blends(), 0).weight, 1.0f), "打断前旧动作权重=1");

    q.StartMotion(1, 10.0f, 0.4f, 0.6f, false, 2, "TapHead", q.Clock());
    Check(q.Size() == 2, "新动作入队后队列有 2 条");
    // 新动作刚入队时 fadeIn 权重为 0，按设计不输出（省一次曲线采样）；
    // 此时只有旧动作在淡出。
    Check(q.Blends().size() == 1, "入队当帧只有旧动作生效");
    Check(Near(q.Blends()[0].weight, 1.0f), "旧动作刚被打断时权重仍为 1");

    // 进入交叉淡入淡出阶段：两条动作同时生效。
    q.Tick(0.2f);
    const std::vector<QueueBlend> blends = q.Blends();
    Check(blends.size() == 2, "两条动作同时生效（交叉淡入淡出）");
    Check(blends[0].motionIndex == 0 && blends[1].motionIndex == 1,
          "队列顺序：旧动作在前、新动作在后");
    Check(blends[0].weight > 0.0f && blends[0].weight < 1.0f, "旧动作正在淡出");
    Check(blends[1].weight > 0.0f && blends[1].weight < 1.0f, "新动作正在淡入");

    // 推进到淡出结束（fadeOutSeconds = 旧动作自身 fadeOut = 0.6）
    q.Tick(0.5f);
    Check(q.Size() == 1, "旧动作淡完出队，只剩新动作");
    Check(FindBlend(q.Blends(), 1).weight > 0.9f, "新动作已淡入完成");
}

void TestLoop() {
    std::printf("-- Meta.Loop 循环 --\n");
    MotionQueue q;
    q.StartMotion(3, 2.0f, 0.0f, 0.0f, true, 0, "Idle", q.Clock());
    q.Tick(5.0f);
    Check(q.Size() == 1, "循环动作不会结束");
    Check(Near(FindBlend(q.Blends(), 3).time, 1.0f), "5s 后时间回绕到 1.0s");
}

void TestFinishAndStop() {
    std::printf("-- FinishPrimary / StopAll --\n");
    MotionQueue q;
    q.StartMotion(0, 10.0f, 0.0f, 0.5f, false, 0, "Idle", q.Clock());
    q.Tick(1.0f);
    q.FinishPrimary();
    q.Tick(0.01f);
    Check(q.Empty(), "FinishPrimary 后主动作出队");
    Check(q.Primary() == nullptr, "无主动作");

    q.StartMotion(1, 10.0f, 0.0f, 0.0f, false, 0, "Idle", q.Clock());
    q.StartMotion(2, 10.0f, 0.0f, 0.0f, false, 2, "Tap", q.Clock());
    Check(q.Size() == 2, "StopAll 前有 2 条");
    q.StopAll();
    Check(q.Empty(), "StopAll 清空队列");
}

void TestFinishedPriority() {
    std::printf("-- 优先级透传 --\n");
    MotionQueue q;
    q.StartMotion(0, 10.0f, 0.0f, 0.0f, false, 0, "Idle", q.Clock());
    q.StartMotion(1, 10.0f, 0.0f, 0.0f, false, 3, "Force", q.Clock());
    const std::vector<QueueBlend> blends = q.Blends();
    Check(blends.back().priority == 3, "Force 优先级透传到 Blend");
    Check(q.Primary()->priority == 3, "Primary 是最后入队的高优先级动作");
}

}  // namespace

int main() {
    std::printf("=== MotionQueue 单元测试 ===\n");
    TestEasingSine();
    TestSingleMotionFade();
    TestNoFade();
    TestCrossFade();
    TestLoop();
    TestFinishAndStop();
    TestFinishedPriority();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
