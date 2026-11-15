// PPet - C++ 移植版
// 窗口缩放 / 渲染倍率（WindowFit.hpp）单元测试。
// 覆盖 Q2：渲染倍率变化时窗口必须等比调整，且不能被桌面限制压扁。
#include <src/tools/core/WindowFit.hpp>

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

using namespace ppet;

void TestScaleWindowSize() {
    std::printf("-- 按渲染倍率等比缩放窗口 --\n");
    // 用户实测场景：1.0x -> 2.0x，437x600 -> 874x1200（桌面足够大）。
    const WindowSize s2 = ScaleWindowSize(437, 600, 2.0f, 160, 200, 4000, 4000);
    Check(s2.width == 874 && s2.height == 1200,
          "1.0x -> 2.0x：窗口等比放大（437x600 -> 874x1200）");
    Check(s2.width * 600 == s2.height * 437, "放大后宽高比保持 437:600");

    // 2.0x -> 1.0x 必须精确还原，避免反复拖动滑块尺寸漂移。
    const WindowSize back = ScaleWindowSize(s2.width, s2.height, 0.5f, 160, 200, 4000, 4000);
    Check(back.width == 437 && back.height == 600,
          "2.0x -> 1.0x：窗口精确还原（往返无漂移）");

    // 0.25x / 4.0x 边界（设置面板滑块范围 25..400）。
    const WindowSize small = ScaleWindowSize(800, 600, 0.25f, 160, 200, 1920, 1080);
    Check(small.width == 200 && small.height == 200,
          "0.25x 时钳制到最小尺寸 200x200，不产生畸形窗口");

    // 无效倍率原样返回。
    const WindowSize bad = ScaleWindowSize(437, 600, 0.0f, 160, 200, 1920, 1080);
    Check(bad.width == 437 && bad.height == 600, "倍率 <= 0 时保持原尺寸");
    const WindowSize neg = ScaleWindowSize(437, 600, -1.0f, 160, 200, 1920, 1080);
    Check(neg.width == 437 && neg.height == 600, "负倍率保持原尺寸");
}

void TestDesktopClamp() {
    std::printf("-- 桌面尺寸限制下保持宽高比 --\n");
    // 1080p 桌面：高度先到极限。不能分别 clamp 宽高，必须整体等比收缩。
    const WindowSize s = ScaleWindowSize(437, 600, 2.0f, 160, 200, 1896, 1056);
    Check(s.height <= 1056 && s.width <= 1896, "窗口不超出桌面可用区域");
    Check(Near(static_cast<float>(s.width) / static_cast<float>(s.height),
               437.0f / 600.0f, 0.02f),
          "被桌面限制时仍整体等比收缩，宽高比不被压扁");
    Check(s.height == 1056, "高度恰好用满可用区域");

    // EffectiveScaleRatio：实际倍率小于目标倍率时用于更新基准。
    const float r = EffectiveScaleRatio(437, 600, s.width, s.height);
    Check(r > 1.0f && r < 2.0f, "桌面限制后实际生效倍率介于 1x 与 2x 之间");
    Check(Near(r, static_cast<float>(s.height) / 600.0f, 0.02f),
          "实际倍率按较小边计算");

    // 往返：用实际倍率作为基准再缩回 1.0x，应回到原始尺寸附近。
    const WindowSize restored =
        ScaleWindowSize(s.width, s.height, 1.0f / r, 160, 200, 1896, 1056);
    Check(std::abs(restored.width - 437) <= 2 && std::abs(restored.height - 600) <= 2,
          "以实际倍率为基准缩回 1.0x 可还原窗口（无累积漂移）");

    Check(Near(EffectiveScaleRatio(0, 600, 800, 600), 1.0f),
          "无效输入返回 1.0");
}

}  // namespace

int main() {
    std::printf("=== WindowFit 单元测试 ===\n");
    TestScaleWindowSize();
    TestDesktopClamp();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
