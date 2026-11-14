// PPet - C++ 移植版
// 视线追踪策略单元测试（需求 7 / Q5）：
//   - 全屏追踪开启 -> 使用全局光标
//   - 全屏追踪关闭且有窗口内光标 -> 使用窗口内光标
//   - 全屏追踪关闭且无窗口内光标 -> 不追踪（None，模型回正）
//   - 最大偏转角度钳制到 [0, 90]
//   - 追踪半径对 Global / Window 两个分支统一生效
//   - 窗口内光标归一化（含半径、越界钳制）
//   - 三类模型后端共用的视线角度上限换算
//
// 纯逻辑、零依赖（不碰窗口 / 追踪器 / GL），三种 CMake 配置均可编译运行。
#include <src/tools/core/TrackingPolicy.hpp>

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

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

}  // namespace

int main() {
    using ppet::ClampTrackingAngle;
    using ppet::EffectiveTrackingRadius;
    using ppet::NormalizeWindowCursor;
    using ppet::ResolveGazeLimits;
    using ppet::ResolveTrackingSource;
    using ppet::TrackingSource;

    std::printf("=== TrackingPolicy 单元测试（需求 7 / Q5）===\n");

    // --- 三分支：全屏追踪开启 ---
    Check(ResolveTrackingSource(true, false) == TrackingSource::Global,
          "全屏开启(无窗口光标) -> Global");
    Check(ResolveTrackingSource(true, true) == TrackingSource::Global,
          "全屏开启(有窗口光标) -> Global");

    // --- 三分支：全屏追踪关闭 ---
    Check(ResolveTrackingSource(false, true) == TrackingSource::Window,
          "全屏关闭(有窗口光标) -> Window");
    Check(ResolveTrackingSource(false, false) == TrackingSource::None,
          "全屏关闭(无窗口光标) -> None");
    // Q5 回归：鼠标离开窗口后 hasWindowCursor 复位，必须变成 None。
    bool hasWindowCursor = true;
    Check(ResolveTrackingSource(false, hasWindowCursor) == TrackingSource::Window,
          "Q5：鼠标在窗口内时用 Window");
    hasWindowCursor = false;  // onMouseLeave 复位
    Check(ResolveTrackingSource(false, hasWindowCursor) == TrackingSource::None,
          "Q5 回归：鼠标离开 + 全屏关闭 -> None（模型回正，不再跟着鼠标）");
    Check(ResolveTrackingSource(false, false) != TrackingSource::Global,
          "需求7回归：关闭全屏后不应使用全局光标");

    // --- 最大偏转角度钳制 ---
    Check(ClampTrackingAngle(30.0f) == 30.0f, "30 不变");
    Check(ClampTrackingAngle(-10.0f) == 0.0f, "负数钳到 0");
    Check(ClampTrackingAngle(120.0f) == 90.0f, "超 90 钳到 90");
    Check(ClampTrackingAngle(0.0f) == 0.0f, "0 合法");
    Check(ClampTrackingAngle(90.0f) == 90.0f, "90 合法");

    // --- 追踪半径：Global / Window 共用同一配置 ---
    Check(Near(EffectiveTrackingRadius(400.0f, 300.0f), 400.0f),
          "配置半径优先（默认 400）");
    Check(Near(EffectiveTrackingRadius(50.0f, 300.0f), 50.0f),
          "小半径也生效：窗口分支不再硬编码半对角线");
    Check(Near(EffectiveTrackingRadius(5000.0f, 300.0f), 5000.0f),
          "大半径生效：追踪变迟钝");
    Check(Near(EffectiveTrackingRadius(0.0f, 300.0f), 300.0f),
          "配置非正时回退窗口半对角线");
    Check(Near(EffectiveTrackingRadius(-5.0f, 0.0f), 1.0f),
          "全部无效给 1，避免除零");

    // --- 窗口内光标归一化 ---
    {
        float nx = 9.0f, ny = 9.0f;
        NormalizeWindowCursor(400.0f, 300.0f, 800.0f, 600.0f, 400.0f, &nx, &ny);
        Check(Near(nx, 0.0f) && Near(ny, 0.0f), "窗口中心归一化到 (0,0)");
    }
    {
        float nx = 0.0f, ny = 0.0f;
        NormalizeWindowCursor(800.0f, 300.0f, 800.0f, 600.0f, 400.0f, &nx, &ny);
        Check(Near(nx, 1.0f), "右边缘 x = +1");
        Check(Near(ny, 0.0f), "水平移动不影响 y");
    }
    {
        float nx = 0.0f, ny = 0.0f;
        NormalizeWindowCursor(400.0f, 0.0f, 800.0f, 600.0f, 400.0f, &nx, &ny);
        Check(Near(ny, 0.75f), "上边缘 y 向上为正（300/400 = 0.75）");
    }
    {
        float nx = 0.0f, ny = 0.0f;
        NormalizeWindowCursor(10000.0f, -10000.0f, 800.0f, 600.0f, 400.0f, &nx, &ny);
        Check(nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f,
              "越界坐标钳制到 [-1,1]");
    }
    {
        float nx = 9.0f, ny = 9.0f;
        NormalizeWindowCursor(400.0f, 300.0f, 800.0f, 600.0f, 0.0f, &nx, &ny);
        Check(Near(nx, 0.0f) && Near(ny, 0.0f), "半径 0 兜底，不除零");
        float keep = 7.0f;
        NormalizeWindowCursor(400.0f, 300.0f, 800.0f, 600.0f, 100.0f, nullptr, &keep);
        Check(Near(keep, 0.0f), "空输出指针安全");
    }

    // --- 视线角度上限（PMX / VRM / Live2D 共用） ---
    {
        const auto g = ResolveGazeLimits(30.0f, 0.5f);
        Check(Near(g.yawRad, 30.0f * kDeg2Rad), "30 度换算为弧度约 0.5236");
        Check(Near(g.pitchRad, 30.0f * kDeg2Rad * 0.5f), "pitch 按比例 0.5");
    }
    {
        const auto g = ResolveGazeLimits(0.0f, 0.5f);
        Check(Near(g.yawRad, 0.0f) && Near(g.pitchRad, 0.0f),
              "角度 0 时完全不偏转（滑块拖到最小生效）");
    }
    {
        const auto g = ResolveGazeLimits(90.0f, 0.5f);
        Check(Near(g.yawRad, 90.0f * kDeg2Rad), "90 度上限");
    }
    {
        const auto g = ResolveGazeLimits(120.0f, 0.5f);
        Check(Near(g.yawRad, 90.0f * kDeg2Rad), "超过 90 度钳制");
    }
    {
        const float modelYaw = 15.0f * kDeg2Rad;
        const auto g = ResolveGazeLimits(30.0f, 0.6f, modelYaw);
        Check(Near(g.yawRad, modelYaw),
              "VRM：模型 lookAt 上限更小时取模型值，不会把头扭过头");
        Check(Near(g.pitchRad, modelYaw * 0.6f), "VRM：pitch 同步按模型比例");
    }
    {
        const auto g = ResolveGazeLimits(10.0f, 0.6f, 30.0f * kDeg2Rad);
        Check(Near(g.yawRad, 10.0f * kDeg2Rad),
              "VRM：配置角度更小时取配置值（调小滑块明显生效）");
    }
    {
        const auto g = ResolveGazeLimits(30.0f, 0.0f);
        Check(Near(g.pitchRad, g.yawRad * 0.5f), "比例 <= 0 时回退 0.5");
    }

    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    if (failures == 0) std::printf("tracking_policy_unit: PASS\n");
    return failures == 0 ? 0 : 1;
}
