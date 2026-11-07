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

    /// 当前虚拟桌面矩形（来自 GlobalCursorTracker，可能为负坐标）。
    IntRect DesktopRect() const;

    /// 把窗口钳制回虚拟桌面可见范围并落位。
    /// 用于：加载配置后、拖动结束、托盘重置后 —— 杜绝「窗口跑到桌面外够不着」。
    void ClampWindowToDesktop();

    /// 托盘「重置窗口位置」：移到 (0,0)（主显示器左上角）并保存配置。
    void ResetWindowPosition();

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

    // 拖动状态：用「抓取点偏移」做绝对定位，避免增量算法的正反馈漂移。
    //
    // 记 W = 窗口左上角（虚拟桌面坐标），L = 光标在窗口内的坐标，则全局光标
    // G = W + L 恒成立（窗口移动会等量反向改变 L）。按下时记录抓取偏移 A = L0，
    // 之后每次 mouse move 直接令 W' = W + L - A = G - A，窗口跟着光标走且不漂移。
    bool dragging_ = false;
    double dragGrabX_ = 0.0;
    double dragGrabY_ = 0.0;

    // 「移动窗口」模式：临时关闭点击穿透，让平时鼠标穿透的窗口也能被拖动。
    // 退出时按「当前配置」恢复穿透，不保存进入时的快照，避免覆盖用户中途的改动。
    bool moveMode_ = false;

    // 窗口是否真的被移动过（用于区分「用户拖动」与「程序设置位置」）。
    bool windowMovedByUser_ = false;
};

}  // namespace ppet
