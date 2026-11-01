// PPet - C++ 移植版
// 设置界面预览：渲染设置面板并把结果保存为 PNG，便于人工确认界面效果。
//
// 用法：ppet_settings_panel_preview <输出.png>
// 需要真实 GL 环境（无显示时以退出码 2 跳过）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// GLEW 必须排在 GLFW 之前。
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "platform/Window.hpp"
#include "ui/SettingsPanel.hpp"

namespace {

/// 读取设置窗口帧缓冲并写出 PNG（stb_image_write 需要 top-down 行序，需翻转）。
bool SavePng(GLFWwindow* window, const char* path) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) return false;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // 上下翻转（glReadPixels 是 bottom-up）。
    const int stride = width * 4;
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < height; ++y) {
        std::memcpy(flipped.data() + static_cast<size_t>(y) * stride,
                    pixels.data() + static_cast<size_t>(height - 1 - y) * stride,
                    static_cast<size_t>(stride));
    }

    return stbi_write_png(path, width, height, 4, flipped.data(), stride) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "settings_panel.png";

    ppet::Window mainWindow;
    ppet::WindowOptions opts;
    opts.width = 400;
    opts.height = 400;
    opts.x = 0;
    opts.y = 0;
    opts.title = "PPet";
    opts.transparent = false;
    opts.resizable = false;
    opts.skipTaskbar = true;

    std::string error;
    if (!mainWindow.Create(opts, error)) {
        std::printf("settings_panel_preview: 主窗口创建失败：%s\n", error.c_str());
        return 2;
    }

    ppet::AppConfig cfg;
    ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);
    cfg.modelPath = "D:/Models/Avatar.vrm";
    cfg.modelList.push_back("D:/Models/Avatar.vrm");
    cfg.modelList.push_back("D:/Models/Hero.pmx");
    cfg.modelList.push_back("D:/Models/Cat.model3.json");
    cfg.fullscreenTracking = true;
    cfg.trackingRadius = 400.0f;
    cfg.maxTrackingAngle = 30.0f;
    cfg.alwaysOnTop = true;
    cfg.showTool = true;

    ppet::SettingsPanel panel;
    panel.config = &cfg;
    panel.strings = &strings;
    panel.onImportModel = [](const std::string&) {};
    panel.onRemoveModel = [](int) {};
    panel.onSelectModel = [](int) {};
    panel.onConfigChanged = []() {};

    if (!panel.Initialize(&mainWindow, error)) {
        std::printf("settings_panel_preview: 设置面板初始化失败：%s\n", error.c_str());
        mainWindow.Destroy();
        return 1;
    }

    panel.Show();
    for (int i = 0; i < 8; ++i) {
        mainWindow.PollEvents();
        panel.Render();
        mainWindow.SwapBuffers();
    }

    glfwMakeContextCurrent(panel.Handle());
    const bool ok = SavePng(panel.Handle(), outPath);
    glfwMakeContextCurrent(mainWindow.Handle());

    std::printf("settings_panel_preview: %s -> %s\n", ok ? "已保存" : "保存失败", outPath);
    panel.Shutdown();
    mainWindow.Destroy();
    return ok ? 0 : 1;
}
