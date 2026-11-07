// PPet - C++ 移植版
// 应用主控制器：管理窗口、托盘、模型生命周期与主循环。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <src/tools/core/Config.hpp>
#include <src/tools/core/I18n.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/model/Model.hpp>
#include <src/tools/model/ModelError.hpp>
#include <src/tools/platform/GlobalCursorTracker.hpp>
#include <src/tools/platform/Tray.hpp>
#include <src/tools/platform/Window.hpp>
#include <src/ui/DebugPanel.hpp>
#include <src/ui/SettingsPanel.hpp>

namespace ppet {

/// 应用主体。
class App {
public:
    App() = default;
    ~App();

    /// 初始化（窗口、托盘、追踪器、配置）。失败时 error 写入原因。
    bool Initialize(std::string& error);

    /// 主循环，直到窗口关闭。
    void Run();

    /// 清理资源。
    void Shutdown();

private:
    /// 加载指定路径的模型（精简版，只取字符串错误，不弹窗）。
    bool LoadModel(const std::string& path, std::string& error);

    /// 加载指定路径的模型（完整版）。
    /// @param error      结构化失败信息（分类 / 明细 / 建议）。
    /// @param notifyUser 是否把失败或隐患用 Qt 弹窗告知用户。
    ///                   启动时的自动加载传 false，避免每次启动弹窗；
    ///                   用户主动导入/切换/重载传 true。
    bool LoadModel(const std::string& path, ModelLoadError& error, bool notifyUser);

    /// 把失败/警告写日志，并在需要时弹 Qt 提示框。
    void ReportModelProblem(const ModelLoadError& error, bool showDialog);

    /// 模型包解压缓存目录（与配置文件同级的 models 子目录）。
    std::string ModelCacheDirectory() const;

    /// 按当前配置重建托盘菜单。
    void RebuildTrayMenu();

    /// 处理托盘菜单回调。
    void OnTrayItem(const std::string& id, bool checked);

    /// 切换到列表中的下一个/上一个模型。
    void SwitchModel(int delta);

    /// 同步窗口状态到配置并保存。
    void SaveConfig();

    /// 应用配置到窗口与追踪器。
    void ApplyConfig();

    /// 配置语言变化时刷新界面文案与托盘菜单（由设置面板回调触发）。
    void RefreshLanguageIfNeeded();

    /// 构建本帧的渲染上下文。
    RenderContext BuildRenderContext(float deltaTime) const;

    /// 每帧处理输入事件。
    void ProcessInput();

    // --- 成员 ---
    Window window_;
    Tray tray_;
    GlobalCursorTracker cursorTracker_;
    ConfigStore configStore_;
    ModelPtr model_;
    Strings strings_;

    std::string configPath_;
    bool running_ = false;
    bool showTool_ = true;
    Language currentLanguage_ = Language::Chinese;
    SettingsPanel settingsPanel_;
    /// 模型调试/诊断面板（托盘菜单「模型诊断」打开）。
    DebugPanel debugPanel_;
    float lastFrameTime_ = 0.0f;

    /// 最近一次导入的模型包信息（单文件导入时为空），供诊断面板显示「来源」。
    ModelPackage lastPackage_;
    bool lastFromZip_ = false;

    /// 最近一次加载的失败/警告信息，供诊断面板「错误与警告（详细）」显示。
    ModelLoadError lastLoadError_;

    /// 帧统计（供诊断面板的 FPS / 帧时）。
    std::uint64_t frameCount_ = 0;
    float frameMsSmoothed_ = 0.0f;

    // 拖动状态：记录鼠标按下时的偏移，避免窗口跳动。
    bool dragging_ = false;
    double dragOffsetX_ = 0.0;
    double dragOffsetY_ = 0.0;

    // 窗口是否真的被移动过（用于区分「用户拖动」与「程序设置位置」）。
    bool windowMovedByUser_ = false;
};

}  // namespace ppet
