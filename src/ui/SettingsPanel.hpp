// PPet - C++ 移植版
// 设置界面：独立的普通窗口 + Dear ImGui。
//
// 【为什么必须是独立窗口】
//   主窗口是「透明无边框的桌宠叠加窗口」，它没有标题栏、不能交互遮挡，
//   并且开着「忽略鼠标点击」时完全不接收输入。把设置界面塞进主窗口会导致：
//     - 桌宠窗口被系统当成无边框工具窗，设置界面的标题栏画不出来；
//     - 「忽略鼠标点击」开启后，设置界面也点不动。
//   因此这里单独创建一个 800x600 的普通窗口（有标题栏、不透明、可调整大小），
//   它有自己的 GL 上下文，与主窗口的上下文互不干扰。
//
// 【多上下文与 ImGui 的关系】
//   Dear ImGui 是「单上下文」的：ImGui::CreateContext() 只应调用一次，
//   整个进程共用一个 ImGui 上下文。而 OpenGL 侧是「多上下文」的：
//   每个 GLFW 窗口有自己的 GL 上下文与状态机。
//   本类的做法是：
//     1. ImGui 上下文全局唯一，渲染只发生在设置窗口的 GL 上下文里；
//     2. ImGui 的 GLFW 后端初始化时绑定设置窗口（而不是主窗口），
//        因此 io.DisplaySize 与鼠标位置都来自设置窗口；
//     3. 主循环每帧先渲染主窗口，再 glfwMakeContextCurrent(设置窗口) 渲染设置界面，
//        渲染完切回主窗口。切换后必须重新设置视口与 GL 状态，
//        这些由本类的 Render() 负责。
//
// 【ImGui GLFW 后端的回调链】
//   ImGui 的 GLFW 后端内部只保存一个 GLFWwindow*（bd->Window），
//   并通过 ImGui_ImplGlfw_SetCallbacksChainForAllWindows(true)
//   让它把事件转发给所有窗口上的既有回调。
//   本项目主窗口（src/platform/Window.cpp）自己注册了 GLFW 回调，
//   因此必须开启该开关，否则主窗口的拖动 / 点击回调会被 ImGui 覆盖掉。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/Config.hpp"
#include "core/I18n.hpp"

struct GLFWwindow;

namespace ppet {

class Window;

/// 设置界面面板。
///
/// 生命周期：
///   Initialize()  创建独立的 GLFW 窗口 + GL 上下文 + ImGui 后端；
///   Show()/Hide() 显示 / 隐藏窗口（隐藏不销毁，保持界面状态）；
///   Render()     每帧在设置窗口的 GL 上下文里绘制界面；
///   Shutdown()   释放全部资源。
///
/// 数据绑定由上层（App）填充：
///   config            指向要编辑的配置对象，面板直接读写它的字段；
///   onConfigChanged   配置被修改后回调（上层负责持久化与生效）；
///   onImportModel     导入模型（本地路径或在线 URL）；
///   onRemoveModel     删除模型列表中的第 index 项；
///   onSelectModel     切换到模型列表中的第 index 项。
class SettingsPanel {
public:
    SettingsPanel();
    ~SettingsPanel();

    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /// 创建独立窗口并初始化 ImGui。
    ///
    /// @param mainWindow 主窗口（可为 nullptr，仅用于多显示器定位参考）
    /// @param error      失败时写入中文原因
    /// @return 成功返回 true
    bool Initialize(Window* mainWindow, std::string& error);

    /// 释放窗口与 ImGui 资源。可重复调用。
    void Shutdown();

    /// 每帧渲染设置界面。
    ///
    /// 内部会 glfwMakeContextCurrent(设置窗口) -> 绘制 -> 切回原上下文，
    /// 因此调用方不需要自己处理上下文切换（但主窗口的绘制仍需在主上下文里进行）。
    void Render();

    /// 窗口当前是否可见。
    bool IsVisible() const;

    /// 显示设置窗口。
    void Show();

    /// 隐藏设置窗口（不销毁，保留界面状态）。
    void Hide();

    /// 可见性取反。
    void Toggle();

    /// 设置窗口的 GLFW 句柄（未创建时为 nullptr）。
    GLFWwindow* Handle() const;

    // -------------------------------------------------------------------------
    // 数据绑定（由 App 层填充）
    // -------------------------------------------------------------------------

    /// 要编辑的配置。为 nullptr 时界面中的编辑控件会被禁用。
    AppConfig* config = nullptr;

    /// 界面文案（本地化）。为 nullptr 时使用内置中文兜底。
    Strings* strings = nullptr;

    /// 配置发生变化（滑块拖动、开关切换等）时回调。
    std::function<void()> onConfigChanged;

    /// 导入模型：参数为本地路径或在线 URL。
    std::function<void(const std::string& path)> onImportModel;

    /// 删除模型列表中的第 index 项。
    std::function<void(int index)> onRemoveModel;

    /// 选中模型列表中的第 index 项（切换当前模型）。
    std::function<void(int index)> onSelectModel;

    /// 「清除设置」：把配置恢复为默认值。
    std::function<void()> onClearSettings;

private:
    /// 绘制界面内容（仅在设置窗口的 GL 上下文里调用）。
    void BuildUi();

    /// 内部实现（把 ImGui 头文件隔离在 .cpp 里）。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ppet
