// PPet - C++ 移植版
// 模型调试/诊断面板：独立的 Qt 6 窗口，实时显示当前模型与渲染状态。
//
// 【它回答什么问题】
//   桌宠是透明无边框窗口，「模型没显示出来」时用户完全无从下手。
//   本面板把黑盒打开，分四块呈现：
//     1. 概览与渲染 —— 模型是谁、从哪来、是否正常渲染（状态灯 + GL 错误）；
//     2. 参数       —— 顶点/三角形/骨骼/材质/贴图/FPS/帧时/视口/光标；
//     3. 模型部分   —— 每个子网格/材质的三角形数与贴图、双面、描边；
//     4. 性质与资源 —— 格式版本、编码、坐标系、缺失贴图清单、日志位置。
//
// 【与主循环的关系】
//   沿用 SettingsPanel 的模式：本类不是 QObject，不需要 moc / AUTOMOC；
//   QApplication 已在 main.cpp 创建，App 每帧调用 PumpEvents() 驱动事件。
//   Update() 只在窗口可见时才刷新，并做 200 ms 节流，不会拖慢渲染。
//
// 【数据来源】
//   模型信息由 IModel::DebugInfo() 提供（各后端填充，新增格式不会崩）；
//   运行时数据由 App 每帧采集成 RuntimeStats 传入。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <src/tools/core/I18n.hpp>
#include <src/tools/core/Math.hpp>
#include <src/tools/model/Model.hpp>

class QWidget;

namespace ppet {

/// 主循环每帧采集的运行时状态。
struct RuntimeStats {
    float fps = 0.0f;
    float frameMs = 0.0f;
    float deltaTime = 0.0f;
    std::uint64_t frameCount = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    float renderScale = 1.0f;
    Vec2 globalCursor{0.0f, 0.0f};
    Vec2 localCursor{0.0f, 0.0f};
    bool hasGlobalCursor = false;
    bool trackerRunning = false;
    unsigned int glError = 0;
    std::string logPath;
    /// 桌面尺寸（虚拟桌面，多显示器时是总范围）。
    int desktopWidth = 0;
    int desktopHeight = 0;
    /// 当前渲染帧的 PNG 路径（App 在面板可见时抓取，供「图片渲染」页预览）。
    std::string renderPreviewPath;
    std::string renderPreviewNote;
};

class DebugPanel {
public:
    DebugPanel();
    ~DebugPanel();

    DebugPanel(const DebugPanel&) = delete;
    DebugPanel& operator=(const DebugPanel&) = delete;

    /// 创建 Qt 对话框与全部控件。要求 QApplication 已在当前线程创建。
    bool Initialize(std::string& error);

    /// 释放窗口资源，可重复调用。
    void Shutdown();

    /// 每帧由 App 调用，驱动 Qt 事件分发。
    void PumpEvents();

    bool IsVisible() const;
    void Show();
    void Hide();
    void Toggle();
    QWidget* Handle() const;

    /// 语言切换后刷新界面文案。
    void Refresh();

    /// 面板是否可见且已经到了该刷新的时刻（200 ms 节流）。
    /// App 用它避免每帧构造诊断快照（含磁盘存在性检查）；只有返回 true 时才
    /// 需要采集 glGetError 并调用 IModel::DebugInfo()。
    bool ShouldRefresh() const;

    /// 用最新快照刷新面板。窗口不可见时立即返回（零开销）。
    /// @param model   由 model_->DebugInfo() 得到；未加载模型传默认构造值即可。
    /// @param runtime App 本帧采集的运行时数据。
    void Update(const ModelDebugInfo& model, const RuntimeStats& runtime);

    /// 「复制诊断摘要」回调（可选）。不设置时按钮仍可用，只写剪贴板。
    std::function<void()> onCopyDiagnostics;

    // 数据绑定（由 App 填充）
    Strings* strings = nullptr;

private:
    void BuildUi();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ppet
