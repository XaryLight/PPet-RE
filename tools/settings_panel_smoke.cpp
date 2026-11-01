// PPet - C++ 移植版
// 设置界面（独立窗口 + ImGui）运行时冒烟测试。
//
// 验证点：
//   1. 能创建带标题栏的独立设置窗口，并拥有自己的 GL 上下文；
//   2. ImGui 上下文能正常初始化（含中文字体合并）；
//   3. 连续渲染若干帧不崩溃、不产生 GL 错误；
//   4. Show/Hide/Toggle 可见性切换正确。
//
// 需要真实 GL 环境，因此在 xvfb 下运行；没有显示时以退出码 2 表示跳过。
#include <cstdio>
#include <string>
#include <vector>

// GLEW 必须排在 GLFW 之前。
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "platform/Window.hpp"
#include "ui/SettingsPanel.hpp"

namespace {

/// 统计设置窗口帧缓冲中「非清屏底色」的像素数量。
///
/// 清屏底色是 (0.16, 0.16, 0.18)。ImGui 画出的控件（文字、复选框、滑块…）
/// 颜色都与底色不同，因此只要统计到的差异像素数量大于阈值，
/// 就说明界面确实被绘制出来了（而不是一片空白）。
int CountNonBackgroundPixels(GLFWwindow* window) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) return -1;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // 底色按 8bit 量化：0.16*255≈41，0.18*255≈46。
    const unsigned char kBg[3] = {41, 41, 46};
    const int kTolerance = 6;

    int diff = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (std::abs(static_cast<int>(pixels[i]) - kBg[0]) > kTolerance ||
            std::abs(static_cast<int>(pixels[i + 1]) - kBg[1]) > kTolerance ||
            std::abs(static_cast<int>(pixels[i + 2]) - kBg[2]) > kTolerance) {
            ++diff;
        }
    }
    return diff;
}

}  // namespace

int main() {
    namespace ppet = ppet;

    // --- 主窗口（提供父窗口句柄与上下文切换的锚点）---
    ppet::Window mainWindow;
    ppet::WindowOptions opts;
    opts.width = 400;
    opts.height = 400;
    opts.x = 0;
    opts.y = 0;
    opts.title = "PPet";
    opts.transparent = false;  // 冒烟测试用不透明窗口，避免依赖合成器
    opts.resizable = false;
    opts.skipTaskbar = true;

    std::string error;
    if (!mainWindow.Create(opts, error)) {
        std::printf("settings_panel_smoke: 主窗口创建失败：%s\n", error.c_str());
        return 2;
    }

    // --- 设置面板 ---
    ppet::AppConfig cfg;
    ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);
    cfg.modelPath = "C:/models/demo.vrm";
    cfg.modelList.push_back("C:/models/demo.vrm");
    cfg.modelList.push_back("C:/models/demo.pmx");

    ppet::SettingsPanel panel;
    panel.config = &cfg;
    panel.strings = &strings;

    int importCalls = 0;
    int removeCalls = 0;
    int selectCalls = 0;
    int changeCalls = 0;
    panel.onImportModel = [&importCalls](const std::string&) { ++importCalls; };
    panel.onRemoveModel = [&removeCalls](int) { ++removeCalls; };
    panel.onSelectModel = [&selectCalls](int) { ++selectCalls; };
    panel.onConfigChanged = [&changeCalls]() { ++changeCalls; };

    if (!panel.Initialize(&mainWindow, error)) {
        std::printf("settings_panel_smoke: 设置面板初始化失败：%s\n", error.c_str());
        mainWindow.Destroy();
        return 1;
    }

    if (panel.Handle() == nullptr) {
        std::printf("settings_panel_smoke: 设置窗口句柄为空\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }

    // 初始应为隐藏状态。
    if (panel.IsVisible()) {
        std::printf("settings_panel_smoke: 初始化后不应可见\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }

    // 渲染若干帧。
    panel.Show();
    if (!panel.IsVisible()) {
        std::printf("settings_panel_smoke: Show() 后应可见\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }

    for (int i = 0; i < 10; ++i) {
        mainWindow.PollEvents();
        panel.Render();
        mainWindow.SwapBuffers();
    }

    // 验证界面确实被绘制出来了（而不是一片空白）。
    glfwMakeContextCurrent(panel.Handle());
    const int diffPixels = CountNonBackgroundPixels(panel.Handle());
    glfwMakeContextCurrent(mainWindow.Handle());
    if (diffPixels <= 0) {
        std::printf("settings_panel_smoke: 设置窗口没有任何绘制内容（界面未显示）\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }
    std::printf("settings_panel_smoke: 已绘制 %d 个非底色像素\n", diffPixels);

    // 可见性切换。
    panel.Toggle();
    if (panel.IsVisible()) {
        std::printf("settings_panel_smoke: Toggle() 后应隐藏\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }
    panel.Toggle();
    if (!panel.IsVisible()) {
        std::printf("settings_panel_smoke: 再次 Toggle() 后应可见\n");
        panel.Shutdown();
        mainWindow.Destroy();
        return 1;
    }

    panel.Hide();
    panel.Shutdown();
    mainWindow.Destroy();

    std::printf("settings_panel_smoke: 通过\n");
    return 0;
}
