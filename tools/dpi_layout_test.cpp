// PPet - C++ 移植版
// 高 DPI 视口解析（DpiLayout.hpp）单元测试（Q4）：
//   - HiDPI 下必须用 framebuffer 物理像素，不能用窗口逻辑尺寸
//   - framebuffer 无效时回退窗口尺寸，两者都无效给 1x1
//   - 内容缩放比推算
//
// 回归目标：x2.13 / x1.06 缩放下窗口客户区固定 840x600，但 GPU 按物理
// 像素渲染，早期用逻辑尺寸算投影导致模型超出窗口被截断。
//
// 纯头文件、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/core/DpiLayout.hpp>

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

using ppet::EstimateContentScale;
using ppet::ResolveViewportSize;

}  // namespace

int main() {
    std::printf("=== DpiLayout 单元测试（Q4）===\n");

    // --- HiDPI：必须用 framebuffer 物理像素 ---
    {
        const auto v = ResolveViewportSize(1789, 1278, 840, 600);
        Check(v.width == 1789 && v.height == 1278,
              "x2.13 缩放：viewport 取 framebuffer 物理像素（不被 840x600 截断）");
    }
    {
        const auto v = ResolveViewportSize(890, 636, 840, 600);
        Check(v.width == 890 && v.height == 636, "x1.06 缩放：同样取 framebuffer 尺寸");
    }
    {
        const auto v = ResolveViewportSize(840, 600, 840, 600);
        Check(v.width == 840 && v.height == 600, "1x 缩放：两者一致，结果不变");
    }

    // --- 回退：framebuffer 无效（窗口最小化 / 上下文未就绪）---
    {
        const auto v = ResolveViewportSize(0, 0, 840, 600);
        Check(v.width == 840 && v.height == 600, "framebuffer 为 0 时回退窗口尺寸");
    }
    {
        const auto v = ResolveViewportSize(-1, 600, 840, 600);
        Check(v.width == 840 && v.height == 600, "单个分量为负时该分量回退");
    }
    {
        const auto v = ResolveViewportSize(0, 0, 0, 0);
        Check(v.width == 1 && v.height == 1,
              "全部无效时给 1x1，避免 glViewport(0,0) 触发 GL_INVALID_VALUE");
    }
    {
        const auto v = ResolveViewportSize(1789, 0, 840, 600);
        Check(v.width == 1789 && v.height == 600, "宽有效、高无效时逐分量回退");
    }

    // --- 缩放比推算 ---
    Check(std::fabs(EstimateContentScale(1789, 840) - 2.13f) < 0.02f,
          "1789/840 约为 2.13");
    Check(std::fabs(EstimateContentScale(890, 840) - 1.06f) < 0.02f,
          "890/840 约为 1.06");
    Check(std::fabs(EstimateContentScale(840, 840) - 1.0f) < 1e-5f,
          "1x 缩放比为 1");
    Check(std::fabs(EstimateContentScale(0, 840) - 1.0f) < 1e-5f,
          "无效输入回退到 1.0");

    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
