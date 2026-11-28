// PPet - C++ 移植版
// 触摸反应变体选择器（TouchReactionSelector）单元测试。
//
// 覆盖：
//   1. 单次触摸 → Normal（保持区域默认映射）
//   2. 多次非同区非交替 → Intensified（放大开心强度）
//   3. 同区域反复 → Fatigued（摇头 + 生气）
//   4. 交替触摸不同区域 → Confused（摇头 + 惊讶）
//   5. 交替优先于加强
#include <src/tools/model/TouchReactionSelector.hpp>

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

using ppet::model::ReactionKind;
using ppet::model::TouchAccumulator;
using ppet::model::TouchReactionSelector;
using ppet::model::TouchRegion;

void TestNormal() {
    TouchAccumulator acc;
    auto mod = acc.Observe("head");
    TouchReactionSelector sel;
    auto r = sel.Select(TouchRegion::Head, mod, acc);
    Check(r.kind == ReactionKind::Normal, "单次应为 Normal");
    Check(std::string(r.motionId) == "nod", "摸头应为 nod");
    Check(!r.expression.empty() && r.expression[0].first == "happy", "摸头应叠加 happy");
    Check(r.influence > 0.0f, "摸头应有表情影响强度");
}

void TestIntensified() {
    TouchAccumulator acc;
    auto mod = acc.Observe("head");
    acc.Tick(0.2f);
    mod = acc.Observe("body");
    acc.Tick(0.2f);
    mod = acc.Observe("leg");  // 三次、非同区连续、不足交替长度
    TouchReactionSelector sel;
    auto r = sel.Select(TouchRegion::Leg, mod, acc);
    Check(r.kind == ReactionKind::Intensified, "三次触摸应为 Intensified");
    // leg 基线没有表情，Intensified 下也不应凭空加表情。
    Check(r.expression.empty(), "leg 无基线表情，加强也不应加表情");
}

void TestFatigued() {
    TouchAccumulator acc;
    auto mod = acc.Observe("head");
    for (int i = 1; i < 4; ++i) {
        acc.Tick(0.2f);
        mod = acc.Observe("head");
    }
    TouchReactionSelector sel;
    auto r = sel.Select(TouchRegion::Head, mod, acc);
    Check(r.kind == ReactionKind::Fatigued, "同区连续 4 次应为 Fatigued");
    Check(std::string(r.motionId) == "shake", "疲劳应为 shake");
    Check(!r.expression.empty() && r.expression[0].first == "angry", "疲劳应叠加 angry");
}

void TestConfused() {
    TouchAccumulator acc;
    auto mod = acc.Observe("head");
    acc.Tick(0.2f);
    mod = acc.Observe("body");
    acc.Tick(0.2f);
    mod = acc.Observe("head");
    acc.Tick(0.2f);
    mod = acc.Observe("body");
    TouchReactionSelector sel;
    auto r = sel.Select(TouchRegion::Body, mod, acc);
    Check(r.kind == ReactionKind::Confused, "交替触摸应为 Confused");
    Check(std::string(r.motionId) == "shake", "困惑应为 shake");
    Check(!r.expression.empty() && r.expression[0].first == "surprised",
          "困惑应叠加 surprised");
}

void TestConfusedOverIntensified() {
    TouchAccumulator acc;
    auto mod = acc.Observe("head");
    acc.Tick(0.2f);
    mod = acc.Observe("body");
    acc.Tick(0.2f);
    mod = acc.Observe("left");
    acc.Tick(0.2f);
    mod = acc.Observe("right");  // 4 次交替，总次数 >= 3 也满足加强
    TouchReactionSelector sel;
    auto r = sel.Select(TouchRegion::Right, mod, acc);
    Check(r.kind == ReactionKind::Confused, "交替优先于加强");
}

}  // namespace

int main() {
    TestNormal();
    TestIntensified();
    TestFatigued();
    TestConfused();
    TestConfusedOverIntensified();

    if (g_failures == 0) {
        std::printf("[OK] TouchReactionSelector 全部用例通过\n");
        return 0;
    }
    std::printf("[FAIL] TouchReactionSelector %d 项断言失败\n", g_failures);
    return 1;
}
