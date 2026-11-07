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
#include <src/tools/agent/DialogueSystem.hpp>
#include <src/tools/render/FrameCapture.hpp>
#include <src/tools/platform/GlobalCursorTracker.hpp>
#include <src/tools/platform/Tray.hpp>
#include <src/tools/platform/Window.hpp>
#include <src/ui/BubbleWidget.hpp>
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

    /// 每帧推进语言系统（台词/气泡/口型），并把口型写入当前模型。
    void TickDialogue(float dt);

    /// 触发一句指定状态的台词（互动/托盘「说一句」共用），并记住情绪供口型偏置。
    void SayState(agent::AgentStateKind kind, int priority = 1);

    /// 配置语言变化时刷新界面文案与托盘菜单（由设置面板回调触发）。
    void RefreshLanguageIfNeeded();

    /// 构建本帧的渲染上下文。
    RenderContext BuildRenderContext(float deltaTime) const;

    /// 读回当前帧缓冲，更新「像素证据」成员。
    /// @param wantPreview 是否同时把当前帧写成 PNG 供诊断面板预览。
    /// 面板可见时每 200 ms 调一次；不可见时每 120 帧调一次做运行时自检，
    /// 这样即使不打开面板也能在日志里发现「draw calls 正常但画面全空」。
    void CollectFrameEvidence(bool wantPreview);

    /// 每帧处理输入事件。
    void ProcessInput();

    // --- 成员 ---
    Window window_;
    Tray tray_;
    GlobalCursorTracker cursorTracker_;
    ConfigStore configStore_;
    ModelPtr model_;
    Strings strings_;

    /// 语言系统：台词库 / 气泡状态机 / 口型同步（PMX/VRM/Live2D 共用）。
    agent::PhraseBook phraseBook_ = agent::DefaultPhraseBook();
    agent::SpeechController speech_{4242u, nullptr};
    agent::LipSync lipSync_;
    std::string currentPhrase_;
    bool bubbleEnabled_ = true;
    /// 最近一次主动触发的台词情绪，供口型嘴形偏置（笑/怒）。待机碎碎念时为 Idle。
    agent::AgentStateKind lastSpeechKind_ = agent::AgentStateKind::Idle;
    /// 台词气泡窗口（默认跟随主窗口，不随 Qt GUI 置顶）。
    BubbleWidget* bubble_ = nullptr;

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

    /// 帧缓冲像素证据：判断「是否真的出图」，不靠 draw calls / GL 错误猜。
    bool hasFrameEvidence_ = false;
    FramePixelStats lastFrameStats_;
    std::uint64_t lastEvidenceFrame_ = 0;
    /// 上一次写日志时的健康状态，用于只在状态翻转时记一条，避免刷日志。
    bool lastEvidenceLoggedHealthy_ = true;
    std::string lastPreviewPath_;
    std::string lastEvidenceError_;

    // 拖动状态：用「抓取点偏移」做绝对定位，避免增量算法的正反馈漂移。
    //
    // 记 W = 窗口左上角（虚拟桌面坐标），L = 光标在窗口内的坐标，则全局光标
    // G = W + L 恒成立（窗口移动会等量反向改变 L）。按下时记录抓取偏移 A = L0，
    // 之后每次 mouse move 直接令 W' = W + L - A = G - A，窗口跟着光标走且不漂移。
    bool dragging_ = false;
    double dragGrabX_ = 0.0;
    double dragGrabY_ = 0.0;

    /// 窗口内鼠标位置（窗口局部坐标）与有效性标记。
    /// 用于全屏追踪不可用时的视线追踪兜底。
    Vec2 windowCursor_{0.0f, 0.0f};
    bool hasWindowCursor_ = false;

    // 「移动窗口」模式：临时关闭点击穿透，让平时鼠标穿透的窗口也能被拖动。
    // 退出时按「当前配置」恢复穿透，不保存进入时的快照，避免覆盖用户中途的改动。
    bool moveMode_ = false;

    // 窗口是否真的被移动过（用于区分「用户拖动」与「程序设置位置」）。
    bool windowMovedByUser_ = false;
};

}  // namespace ppet
