// PPet - C++ 移植版
// 窗口位置钳制（Math.hpp 的 ClampRectInside）单元测试。
//
// 背景：透明置顶无边框窗口一旦被拖到桌面外就没有任何可点的地方，而配置里
// 保存的坐标会让下次启动继续不可见。该函数是所有「程序化设置位置」的守门员。
//
// 覆盖：桌面内不变、跑出四边、负坐标虚拟桌面（多显示器）、窗口比桌面大、
// 桌面几何无效、尺寸保持不变。
#include <cstdio>
#include <string>

#include <src/tools/core/Math.hpp>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

void CheckRect(const ppet::IntRect& got, int x, int y, int w, int h,
               const std::string& what) {
    Check(got.x == x && got.y == y && got.width == w && got.height == h,
          what + " -> (" + std::to_string(got.x) + "," + std::to_string(got.y) +
              " " + std::to_string(got.width) + "x" + std::to_string(got.height) + ")");
}

}  // namespace

int main() {
    using ppet::ClampRectInside;
    using ppet::IntRect;

    // 典型单屏 1920x1080。
    const IntRect desktop{0, 0, 1920, 1080};

    std::printf("== 桌面内：原样返回 ==\n");
    CheckRect(ClampRectInside(IntRect{100, 100, 350, 600}, desktop),
              100, 100, 350, 600, "正常位置");
    CheckRect(ClampRectInside(IntRect{0, 0, 350, 600}, desktop),
              0, 0, 350, 600, "左上角 (0,0)");
    CheckRect(ClampRectInside(IntRect{1570, 480, 350, 600}, desktop),
              1570, 480, 350, 600, "右下角边界（恰好贴合）");

    std::printf("== 跑出桌面：钳回可见范围 ==\n");
    // 这正是「Win+Tab 能看到却够不着」的坐标形态。
    CheckRect(ClampRectInside(IntRect{6400, 3200, 350, 600}, desktop),
              1570, 480, 350, 600, "位置被翻倍放大到屏幕外");
    CheckRect(ClampRectInside(IntRect{-800, -400, 350, 600}, desktop),
              0, 0, 350, 600, "负坐标（主屏左上之外）");
    CheckRect(ClampRectInside(IntRect{-20, 5000, 350, 600}, desktop),
              0, 480, 350, 600, "左边越界 + 下边越界同时发生");

    std::printf("== 多显示器：虚拟桌面带负坐标 ==\n");
    // 左侧有一块 1920 宽的副屏时，虚拟桌面为 x=-1920..1920。
    const IntRect dual{-1920, 0, 3840, 1080};
    CheckRect(ClampRectInside(IntRect{-1900, 100, 350, 600}, dual),
              -1900, 100, 350, 600, "副屏左上角合法");
    CheckRect(ClampRectInside(IntRect{-3000, 100, 350, 600}, dual),
              -1920, 100, 350, 600, "超出副屏左边界");
    // 虚拟桌面 x 范围为 -1920..1920，窗口宽 350，最大合法 x = 1920-350 = 1570。
    CheckRect(ClampRectInside(IntRect{9000, 100, 350, 600}, dual),
              1570, 100, 350, 600, "超出主屏右边界");
    CheckRect(ClampRectInside(IntRect{0, 0, 350, 600}, dual),
              0, 0, 350, 600, "重置到 (0,0) 仍在可见范围内");

    std::printf("== 退化情形 ==\n");
    CheckRect(ClampRectInside(IntRect{50, 60, 2500, 1600}, desktop),
              0, 0, 2500, 1600, "窗口比桌面大：退回桌面左上角且尺寸不变");
    CheckRect(ClampRectInside(IntRect{6400, 3200, 350, 600}, IntRect{0, 0, 0, 0}),
              6400, 3200, 350, 600, "桌面几何无效：原样返回，不强行搬家");
    CheckRect(ClampRectInside(IntRect{6400, 3200, 350, 600}, IntRect{0, 0, -1, 1080}),
              6400, 3200, 350, 600, "桌面宽度为负：原样返回");

    std::printf("\nwindow_layout_test: %d 项检查，%d 项失败\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
