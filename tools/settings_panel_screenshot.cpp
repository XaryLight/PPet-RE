// PPet - 设置面板截图工具（Qt 6）
//
// 用真实 Qt 控件渲染设置面板，并抓取为 PNG，用于 README 配图。
// 用法：ppet_settings_panel_screenshot <输出.png>
//
// 该工具需要 QApplication，因此在 offscreen 平台下也能渲染控件并抓取像素
// （Qt 的栅格绘制引擎会把 widget 绘制到 QPixmap，不依赖真实窗口管理器）。
#include <cstdio>
#include <filesystem>

#include <QApplication>
#include <QPixmap>
#include <QWidget>

#include <src/tools/core/Config.hpp>
#include <src/tools/core/I18n.hpp>
#include <src/ui/SettingsPanel.hpp>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法：%s <输出.png>\n", argv[0]);
        return 1;
    }

    QApplication app(argc, argv);

    ppet::AppConfig cfg;
    cfg.modelList = {"Hiyori.pmx", "Alicia.vrm", "Tokyo7th.vrm"};
    cfg.modelPath = cfg.modelList.front();
    cfg.renderScale = 1.0f;
    cfg.fullscreenTracking = true;
    cfg.trackingRadius = 400.0f;
    cfg.maxTrackingAngle = 30.0f;
    cfg.alwaysOnTop = true;
    cfg.ignoreMouseEvents = false;
    cfg.showTool = true;
    cfg.language = ppet::Language::Chinese;

    ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);

    ppet::SettingsPanel panel;
    std::string error;
    if (!panel.Initialize(error)) {
        std::fprintf(stderr, "初始化设置面板失败：%s\n", error.c_str());
        return 2;
    }
    panel.config = &cfg;
    panel.strings = &strings;
    panel.onConfigChanged = [] {};
    panel.onImportModel = [](const std::string&) {};
    panel.onRemoveModel = [](int) {};
    panel.onSelectModel = [](int) {};
    panel.onClearSettings = [] {};
    panel.Refresh();
    panel.Show();
    app.processEvents();

    QWidget* w = panel.Handle();
    if (!w) {
        std::fprintf(stderr, "设置面板 Handle() 为空\n");
        return 3;
    }
    w->adjustSize();
    app.processEvents();

    QPixmap pm = w->grab();
    if (pm.isNull()) {
        std::fprintf(stderr, "grab() 返回空图像\n");
        return 5;
    }
    // 确保输出目录存在（git 不保留空目录）。
    const std::filesystem::path outPath(argv[1]);
    if (auto parent = outPath.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const bool ok = pm.save(argv[1]);
    if (!ok) {
        std::fprintf(stderr, "保存 PNG 失败：%s\n", argv[1]);
        return 4;
    }
    std::printf("已保存 %s（%dx%d）\n", argv[1], pm.width(), pm.height());
    return 0;
}
