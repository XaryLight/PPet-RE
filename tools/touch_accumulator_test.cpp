// PPet - C++ 移植版
// 交互累积器（TouchAccumulator）单元测试。
//
// 覆盖：
//   1. 首次触摸的基线修饰符
//   2. 防抖：间隔内的重复触摸不计入
//   3. 强度随次数增长并封顶
//   4. 反应变体：常规 → 加强 → 疲劳
//   5. 换区域打断同区连续
//   6. 滑动窗口过期淘汰
//   7. Reset 与非法 dt
#include <src/tools/model/TouchAccumulator.hpp>

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

using ppet::model::TouchAccumulator;
using ppet::model::TouchModifier;

void TestFirstTouch() {
    TouchAccumulator acc;
    TouchModifier m = acc.Observe("head");
    Check(m.totalInWindow == 1, "首次触摸计数应为 1");
    Check(m.sameRegionStreak == 1, "同区连续应为 1");
    Check(std::fabs(m.intensity - 1.0f) < 1e-6f, "首次强度应为 1.0");
    Check(m.variant == 0, "首次变体应为常规");
    Check(acc.Count() == 1, "Count 应为 1");
}

void TestDebounce() {
    TouchAccumulator acc;
    acc.Observe("head");
    TouchModifier m = acc.Observe("head");
    Check(acc.Count() == 1, "防抖间隔内的重复触摸不应计数");
    Check(m.totalInWindow == 1, "返回的计数仍为 1");

    acc.Tick(0.2f);
    m = acc.Observe("head");
    Check(acc.Count() == 2, "超过防抖间隔应计数");
    Check(std::fabs(m.intensity - 1.15f) < 1e-5f, "第二次强度应为 1.15");
}

void TestVariantUpgrade() {
    TouchAccumulator acc;
    acc.Observe("head");
    acc.Tick(0.2f);
    acc.Observe("head");
    acc.Tick(0.2f);
    TouchModifier m = acc.Observe("head");
    Check(m.totalInWindow == 3, "第三次应计数");
    Check(m.variant == 1, "三次触摸应进入加强变体");

    acc.Tick(0.2f);
    m = acc.Observe("head");
    Check(m.sameRegionStreak == 4, "同区连续应为 4");
    Check(m.variant == 2, "同区连续 4 次应进入疲劳变体");
}

void TestDifferentRegionBreaksStreak() {
    TouchAccumulator acc;
    acc.Observe("head");
    acc.Tick(0.2f);
    acc.Observe("head");
    acc.Tick(0.2f);
    TouchModifier m = acc.Observe("body");
    Check(m.sameRegionStreak == 1, "换区域应打断同区连续");
    Check(m.variant == 1, "总次数 3 仍为加强变体");
}

void TestWindowExpiry() {
    TouchAccumulator acc;
    acc.Observe("head");
    acc.Tick(0.2f);
    acc.Observe("body");
    Check(acc.Count() == 2, "窗口内应有 2 条");
    acc.Tick(3.5f);
    Check(acc.Count() == 0, "超过窗口长度应全部过期");
    TouchModifier m = acc.Observe("head");
    Check(m.totalInWindow == 1, "过期后重新计数");
    Check(m.variant == 0, "过期后回到常规变体");
}

void TestIntensityCap() {
    TouchAccumulator acc;
    for (int i = 0; i < 30; ++i) {
        if (i > 0) acc.Tick(0.2f);
        acc.Observe("head");
    }
    TouchModifier m = acc.Observe("head");
    Check(m.intensity <= 2.0f + 1e-5f, "强度不应超过上限 2.0");
    Check(m.intensity >= 1.99f, "高频触摸应达到强度上限");
}

void TestResetAndInvalidDt() {
    TouchAccumulator acc;
    acc.Observe("head");
    acc.Reset();
    Check(acc.Count() == 0, "Reset 应清空记录");
    TouchModifier m = acc.Observe("head");
    Check(m.totalInWindow == 1, "Reset 后重新计数");

    acc.Tick(-1.0f);
    Check(acc.Count() == 1, "负 dt 不应淘汰记录");
}

void TestRegionConversion() {
    using ppet::model::TouchRegion;
    Check(ppet::model::TouchRegionFromString("head") == TouchRegion::Head, "head 应映射 Head");
    Check(ppet::model::TouchRegionFromString("body") == TouchRegion::Body, "body 应映射 Body");
    Check(ppet::model::TouchRegionFromString("left") == TouchRegion::Left, "left 应映射 Left");
    Check(ppet::model::TouchRegionFromString("right") == TouchRegion::Right, "right 应映射 Right");
    Check(ppet::model::TouchRegionFromString("leg") == TouchRegion::Leg, "leg 应映射 Leg");
    Check(ppet::model::TouchRegionFromString("unknown") == TouchRegion::Other, "未知应映射 Other");
    Check(std::string(ppet::model::TouchRegionName(TouchRegion::Head)) == "head",
          "Name 应回写 head");
}

void TestEnumObserve() {
    TouchAccumulator acc;
    acc.Observe(ppet::model::TouchRegion::Head);
    acc.Tick(0.2f);
    TouchModifier m = acc.Observe(ppet::model::TouchRegion::Head);
    Check(acc.Count() == 2, "枚举 Observe 应计数");
    Check(m.sameRegionStreak == 2, "枚举 Observe 同区连续应为 2");
}

void TestQueries() {
    TouchAccumulator acc;
    acc.Observe("head", 0.5f);
    acc.Tick(0.2f);
    acc.Observe("body", 1.0f);
    acc.Tick(0.2f);
    acc.Observe("head", 0.8f);

    Check(acc.CountInRegion(ppet::model::TouchRegion::Head, 3.0f) == 2,
          "Head 区域 3 秒内应 2 次");
    Check(acc.CountInRegion(ppet::model::TouchRegion::Body, 3.0f) == 1,
          "Body 区域 3 秒内应 1 次");
    Check(acc.LastRegion() == ppet::model::TouchRegion::Head, "LastRegion 应为 Head");
    Check(std::fabs(acc.CumulativeIntensity() - 2.3f) < 1e-5f, "累计强度应为 2.3");

    auto recent = acc.RecentRegions(2);
    Check(recent.size() == 2, "RecentRegions 应返回 2 个");
    Check(recent[0] == ppet::model::TouchRegion::Body, "倒数第 2 个应为 Body");
    Check(recent[1] == ppet::model::TouchRegion::Head, "最后 1 个应为 Head");
}

void TestAlternating() {
    TouchAccumulator acc;
    acc.Observe("head");
    acc.Tick(0.2f);
    acc.Observe("body");
    acc.Tick(0.2f);
    acc.Observe("head");
    acc.Tick(0.2f);
    acc.Observe("body");
    Check(acc.IsAlternating(), "head→body→head→body 应判定交替");

    acc.Tick(0.2f);
    acc.Observe("body");
    Check(!acc.IsAlternating(), "尾部 body→body 相邻相同不应判定交替");
}

}  // namespace

int main() {
    TestFirstTouch();
    TestDebounce();
    TestVariantUpgrade();
    TestDifferentRegionBreaksStreak();
    TestWindowExpiry();
    TestIntensityCap();
    TestResetAndInvalidDt();
    TestRegionConversion();
    TestEnumObserve();
    TestQueries();
    TestAlternating();

    if (g_failures == 0) {
        std::printf("[OK] TouchAccumulator 全部用例通过\n");
        return 0;
    }
    std::printf("[FAIL] TouchAccumulator %d 项断言失败\n", g_failures);
    return 1;
}
