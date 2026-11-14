// PPet - C++ 移植版
// 真实内容帧 / 动态像素比（Live2DFrame.hpp）单元测试。
//
// 覆盖需求 3：模型真实渲染像素 2000x600、窗口 600x600 时横向不能截断。
// 纯逻辑、零依赖，可在无 GL / 无 Cubism SDK 环境运行。
#include <api/live2d/Live2DFrame.hpp>

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
bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using namespace ppet::live2d;

void TestContentFrameBuilder() {
    std::printf("-- 内容包围盒累加 --\n");
    ContentFrameBuilder b;
    Check(!b.Frame().valid, "空 builder 无效");

    // 一个宽幅部件：x[-10,10] y[-3,3]（模拟 2000x600 的真实内容）。
    const float wide[] = {-10, -3, 10, -3, 10, 3, -10, 3};
    b.AddDrawable(wide, 4, true);
    Check(b.Frame().valid, "加入可见部件后有效");
    Check(Near(b.Frame().Width(), 20.0f) && Near(b.Frame().Height(), 6.0f),
          "宽 20 高 6");
    Check(Near(b.Frame().Aspect(), 20.0f / 6.0f), "宽高比 3.333");
    Check(Near(b.Frame().CenterX(), 0.0f) && Near(b.Frame().CenterY(), 0.0f),
          "居中内容中心为 0");

    // 偏心内容：中心 (5, 8)。
    ContentFrameBuilder off;
    const float tri[] = {4, 7, 6, 7, 5, 9};
    off.AddDrawable(tri, 3, true);
    Check(Near(off.Frame().CenterX(), 5.0f) && Near(off.Frame().CenterY(), 8.0f),
          "偏心内容中心正确（居中 fit 的依据）");

    // 无贴图纯遮罩（include=false）不得撑大包围盒。
    ContentFrameBuilder masked;
    const float huge[] = {-1000, -1000, 1000, 1000};
    masked.AddDrawable(wide, 4, true);
    masked.AddDrawable(huge, 2, false);
    Check(Near(masked.Frame().Width(), 20.0f), "跳过纯遮罩后包围盒不变");

    // NaN / Inf 顶点不得污染包围盒。
    ContentFrameBuilder nan;
    const float bad[] = {0, 0, NAN, INFINITY, 2, 2};
    nan.AddDrawable(bad, 3, true);
    Check(Near(nan.Frame().maxX, 2.0f) && Near(nan.Frame().maxY, 2.0f),
          "非有限顶点被忽略");
}

void TestFitWindowSize() {
    std::printf("-- 动态像素比（窗口自适应）--\n");
    // 用户场景：真实内容 2000x600 => aspect 3.333，窗口高 600。
    const float aspect = 2000.0f / 600.0f;
    const int w = FitWindowWidth(aspect, 600);
    Check(w == 2000, "高 600 时窗口宽自动变 2000（不截断横向内容）");
    Check(FitWindowWidth(0.0f, 600) == 0, "非法 aspect 返回 0");
    Check(FitWindowWidth(100.0f, 600) == 0, "超范围 aspect 返回 0");
    // 竖向内容 600x2000 => aspect 0.3，窗口宽 600 时高度自适应。
    const int h = FitWindowHeight(0.3f, 600);
    Check(h == 2000, "竖向内容窗口高度自适应");
    // 上限保护。
    Check(FitWindowWidth(3.333f, 5000, 64, 2048) == 2048, "宽度上限钳制生效");
}

void TestFitMatrixNoClipping() {
    std::printf("-- contain fit 不截断 + 居中 --\n");
    // 宽幅内容 x[-10,10] y[-3,3]，窗口 600x600（用户说的方形框）。
    ContentFrame f;
    f.minX = -10; f.maxX = 10; f.minY = -3; f.maxY = 3; f.valid = true;
    float m[16];
    Check(ComputeFitMatrix(f, 600.0f, 600.0f, 1.0f, 1.0f, m), "有效帧返回 true");

    auto xf = [&](float x, float y, float& ox, float& oy) {
        // 列主序：m[col*4+row]；这里 z=0,w=1。
        ox = m[0] * x + m[4] * y + m[12];
        oy = m[1] * x + m[5] * y + m[13];
    };
    float x0, y0, x1, y1;
    xf(-10, -3, x0, y0);
    xf(10, 3, x1, y1);
    Check(x0 >= -1.0001f && x1 <= 1.0001f && y0 >= -1.0001f && y1 <= 1.0001f,
          "四角都落在 NDC [-1,1] 内（不截断）");
    Check(Near(x0, -1.0f) && Near(x1, 1.0f), "横向铺满（contain 取较小边）");
    Check(y1 - y0 < 1.0f, "纵向留白（宽幅内容在方窗口里上下留边）");
    Check(Near(x0 + x1, 0.0f) && Near(y0 + y1, 0.0f), "内容在视口居中");

    // 偏心内容中心 (5,8) 也必须被平移到视口中心。
    ContentFrame off;
    off.minX = 4; off.maxX = 6; off.minY = 7; off.maxY = 9; off.valid = true;
    float mo[16];
    Check(ComputeFitMatrix(off, 400.0f, 400.0f, 1.0f, 1.0f, mo), "偏心帧有效");
    float cx = mo[0] * 5.0f + mo[4] * 8.0f + mo[12];
    float cy = mo[1] * 5.0f + mo[5] * 8.0f + mo[13];
    Check(Near(cx, 0.0f) && Near(cy, 0.0f), "偏心内容中心被移到视口中心");

    // 非法帧 / 非法视口回退 false。
    float bad[16];
    Check(!ComputeFitMatrix(ContentFrame{}, 600, 600, 1, 1, bad), "空帧返回 false");
    Check(!ComputeFitMatrix(f, 0, 600, 1, 1, bad), "零宽视口返回 false");
    Check(Near(bad[0], 1.0f) && Near(bad[5], 1.0f) && Near(bad[12], 0.0f),
          "失败时输出单位矩阵（调用方可安全回退）");
}

void TestAspectDeviation() {
    std::printf("-- 内容/画布宽高比偏差诊断 --\n");
    Check(Near(AspectDeviation(3.333f, 1.0f), 2.333f, 0.01f),
          "偏差 233%（说明必须用内容帧而非画布）");
    Check(Near(AspectDeviation(0.0f, 1.0f), 0.0f), "无效输入返回 0");
}

}  // namespace

int main() {
    std::printf("=== Live2DFrame 单元测试 ===\n");
    TestContentFrameBuilder();
    TestFitWindowSize();
    TestFitMatrixNoClipping();
    TestAspectDeviation();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
