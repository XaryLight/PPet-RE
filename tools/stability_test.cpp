// PPet - C++ 移植版
// 长期运行稳定性单测：帧计数饱和、无符号差值、dt 钳制、相位精度。
#include <src/tools/core/Stability.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    using namespace ppet::stability;
    std::printf("== 长期运行稳定性单测 ==\n");

    const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    Check(SaturatingIncrement(0) == 1, "饱和递增：0 -> 1");
    Check(SaturatingIncrement(41) == 42, "饱和递增：普通值 +1");
    Check(SaturatingIncrement(kMax) == kMax, "饱和递增：到上限后不回绕成 0");

    Check(SafeDelta(10, 3) == 7, "安全差值：正常相减");
    Check(SafeDelta(3, 10) == 0, "安全差值：计数器回退时不下溢");
    Check(SafeDelta(kMax, 0) == kMax, "安全差值：大数仍正确");

    Check(ClampDelta(0.016f) == 0.016f, "dt 钳制：正常帧间隔保留");
    Check(ClampDelta(-1.0f) == 0.0f, "dt 钳制：负数归零");
    Check(ClampDelta(std::numeric_limits<float>::quiet_NaN()) == 0.0f,
          "dt 钳制：NaN 归零");
    Check(ClampDelta(std::numeric_limits<float>::infinity()) == 0.0f,
          "dt 钳制：Inf 归零");
    Check(ClampDelta(30.0f) == 0.1f, "dt 钳制：切后台回来的超大 dt 被限幅");

    // 相位：一个周期后应回到起点；运行 30 年后仍有周期级精度。
    const double freq = 0.24;
    const double period = 1.0 / freq;
    const float p0 = PhaseOf(0.0, freq);
    const float p1 = PhaseOf(period, freq);
    Check(std::fabs(p0 - p1) < 1e-4f, "相位：整周期后回到起点");

    const double thirtyYears = 30.0 * 365.25 * 24.0 * 3600.0;
    const float far = PhaseOf(thirtyYears + 0.37 * period, freq);
    const float near = PhaseOf(0.37 * period, freq);
    Check(std::fabs(far - near) < 2e-3f,
          "相位：连续运行 30 年后仍保持 1e-3 弧度级精度（float 累加会退化）");

    // 对照：float 只有 24 位尾数，30 年（≈9.5e8 秒）时相邻可表示值已差 64 秒，
    // 乘以 0.24Hz*2π 后相位分辨率约 96 弧度，待机动画必然跳帧 —— 这就是必须用 double 的原因。
    const float asFloat = static_cast<float>(thirtyYears);
    const float ulp =
        std::nextafter(asFloat, std::numeric_limits<float>::infinity()) - asFloat;
    Check(ulp > 1.0f, "对照：30 年量级的 float 时间分辨率已退化到秒级以上");

    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
