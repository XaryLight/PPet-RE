// PPet - C++ 移植版
// 窗口坐姿控制器单元测试（缺陷 4）：纯逻辑、零依赖。
//   - 靠近上边缘防抖坐下 / 离开防抖站起；
//   - 窗口丢失立即站起；
//   - 锚点跟随窗口移动；
//   - 边沿标志（JustSat/JustStood）只在当帧有效。
#include <src/tools/model/WindowSittingController.hpp>

#include <cmath>
#include <cstdio>

namespace {

using ppet::model::SitTarget;
using ppet::model::WindowSittingController;
using ppet::Vec2;

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    WindowSittingController::Config cfg;
    cfg.proximityPx = 48.0f;
    cfg.settleSeconds = 0.45f;
    cfg.leaveSeconds = 0.70f;
    WindowSittingController ctrl(cfg);

    // 目标窗口：上边缘 y=100，中心 x=300，宽 400 高 300。
    const SitTarget win{100.0f, 100.0f, 400.0f, 300.0f, true};

    // --- 光标远离上边缘：不坐 ---
    ctrl.Update(win, Vec2(300.0f, 400.0f), 1.0f);
    check(!ctrl.IsSitting(), "光标远离上边缘不坐");
    check(!ctrl.JustSat() && !ctrl.JustStood(), "无坐无站时无边沿标志");

    // --- 靠近上边缘，防抖未到：仍不坐 ---
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.2f);
    check(!ctrl.IsSitting(), "靠近 0.2s（< settle 0.45s）不坐");

    // --- 继续靠近，跨过防抖：坐下 ---
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.3f);  // 累计 0.5s
    check(ctrl.IsSitting(), "累计靠近 0.5s 后坐下");
    check(ctrl.JustSat(), "坐下帧 JustSat 为真");
    check(!ctrl.JustStood(), "坐下帧 JustStood 为假");

    // --- 锚点 = 目标窗口上边缘中点 ---
    check(std::fabs(ctrl.AnchorCenterX() - 300.0f) < 1e-4f, "锚点 X = 窗口上边缘中点");
    check(std::fabs(ctrl.AnchorTopY() - 100.0f) < 1e-4f, "锚点 Y = 窗口上边缘");

    // --- 下一帧边沿标志复位 ---
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.01f);
    check(!ctrl.JustSat() && !ctrl.JustStood(), "下一帧边沿标志复位");

    // --- 窗口移动：锚点跟随 ---
    const SitTarget moved{150.0f, 80.0f, 400.0f, 300.0f, true};
    ctrl.Update(moved, Vec2(350.0f, 80.0f), 0.01f);
    check(ctrl.IsSitting(), "窗口移动时仍坐");
    check(std::fabs(ctrl.AnchorCenterX() - 350.0f) < 1e-4f &&
              std::fabs(ctrl.AnchorTopY() - 80.0f) < 1e-4f,
          "锚点跟随窗口移动");

    // --- 光标离开，防抖未到：仍坐 ---
    ctrl.Update(moved, Vec2(350.0f, 400.0f), 0.5f);
    check(ctrl.IsSitting(), "离开 0.5s（< leave 0.7s）仍坐");

    // --- 继续离开，跨过防抖：站起 ---
    ctrl.Update(moved, Vec2(350.0f, 400.0f), 0.3f);  // 累计 0.8s
    check(!ctrl.IsSitting(), "累计离开 0.8s 后站起");
    check(ctrl.JustStood(), "站起帧 JustStood 为真");
    check(!ctrl.JustSat(), "站起帧 JustSat 为假");
    check(!ctrl.Anchor().valid, "站起后锚点清空");

    // --- 窗口丢失：立即站起（不等待 leaveSeconds） ---
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.5f);
    check(ctrl.IsSitting(), "重新坐下");
    const SitTarget lost{0.0f, 0.0f, 0.0f, 0.0f, false};
    ctrl.Update(lost, Vec2(300.0f, 100.0f), 0.01f);
    check(!ctrl.IsSitting(), "窗口丢失立即站起");
    check(ctrl.JustStood(), "窗口丢失站起帧 JustStood 为真");

    // --- 负 dt / 零 dt 不推进 ---
    ctrl.Reset();
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.4f);
    check(!ctrl.IsSitting(), "累计 0.4s 未坐");
    ctrl.Update(win, Vec2(300.0f, 100.0f), -1.0f);
    check(!ctrl.IsSitting(), "负 dt 不推进防抖");
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.0f);
    check(!ctrl.IsSitting(), "零 dt 不推进防抖");
    ctrl.Update(win, Vec2(300.0f, 100.0f), 0.1f);  // 累计 0.5s
    check(ctrl.IsSitting(), "有效 dt 继续推进后坐下");

    // --- Reset 清空一切 ---
    ctrl.Reset();
    check(!ctrl.IsSitting() && !ctrl.JustSat() && !ctrl.JustStood(),
          "Reset 清空坐姿状态与边沿标志");

    if (failures == 0) {
        std::printf("window_sitting_controller_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("window_sitting_controller_unit: %d FAILED\n", failures);
    return 1;
}
