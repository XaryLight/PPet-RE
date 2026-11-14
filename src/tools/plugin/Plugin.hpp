// PPet - C++ 移植版
// 插件系统：接口与事件定义（需求 6）。
//
// 设计目标：
//   - 轻量、零外部依赖（不碰 Qt / GL / Cubism），三种 CMake 配置均可编译；
//   - 插件通过「事件总线」感知应用行为（模型加载 / 动作播放 / 配置变更 / 互动 / 帧心跳），
//     无需直接耦合 App 内部状态；
//   - 同一套接口既支持「代码内注册的内置插件」，也为将来「扫描目录加载 .so / .dll」留好扩展点；
//   - 纯接口 + 事件，适合单元测试（见 tools/plugin_test.cpp）。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ppet {

struct AppConfig;  // 前向声明，避免插件层依赖配置实现细节

namespace plugin {

/// 插件事件类型。
enum class PluginEventKind {
    AppStart,       ///< 应用启动完成
    AppShutdown,    ///< 应用即将退出
    ModelLoaded,    ///< 模型加载成功（payload: path）
    ModelUnloaded,  ///< 模型卸载
    MotionPlayed,   ///< 触发了一个动作（payload: id）
    MotionStopped,  ///< 停止动作（回到待机）
    ConfigChanged,  ///< 配置变化（payload: key=值 列表）
    Interaction,    ///< 用户点击 / 触摸了模型（payload: region）
    FrameTick,      ///< 节流后的每帧心跳（payload: fps / frame）
};

/// 插件事件。
struct PluginEvent {
    PluginEventKind kind = PluginEventKind::AppStart;
    std::string source;  ///< 触发源（模型路径 / 动作名 / 区域名等）
    std::map<std::string, std::string> payload;  ///< 任意结构化数据
    double timestamp = 0.0;  ///< 应用启动至今的秒数
};

/// 插件宿主：向插件暴露「主动发事件」「读配置」的能力。
class IPluginHost {
public:
    virtual ~IPluginHost() = default;

    /// 主动广播一个事件（所有关心的已启用插件都会收到）。
    virtual void Emit(const PluginEvent& event) = 0;

    /// 当前应用配置（只读快照）。
    virtual const AppConfig& Config() const = 0;
};

/// 插件接口。所有插件实现它。
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// 插件名（唯一标识，命令 `plugin enable/disable <name>` 用）。
    virtual std::string Name() const = 0;

    /// 该插件关心的事件类型；返回空表示「接收全部事件」。
    virtual std::vector<PluginEventKind> Interests() const { return {}; }

    /// 插件被启用时调用（host 可用于主动发事件）。
    virtual void OnEnable(IPluginHost& host) { (void)host; }

    /// 插件被禁用时调用。
    virtual void OnDisable() {}

    /// 收到（其关心的）事件时调用。
    virtual void OnEvent(const PluginEvent& event) { (void)event; }
};

using PluginPtr = std::shared_ptr<IPlugin>;

/// 事件类型可读名（日志 / 控制台用）。
inline const char* PluginEventKindName(PluginEventKind kind) {
    switch (kind) {
        case PluginEventKind::AppStart: return "AppStart";
        case PluginEventKind::AppShutdown: return "AppShutdown";
        case PluginEventKind::ModelLoaded: return "ModelLoaded";
        case PluginEventKind::ModelUnloaded: return "ModelUnloaded";
        case PluginEventKind::MotionPlayed: return "MotionPlayed";
        case PluginEventKind::MotionStopped: return "MotionStopped";
        case PluginEventKind::ConfigChanged: return "ConfigChanged";
        case PluginEventKind::Interaction: return "Interaction";
        case PluginEventKind::FrameTick: return "FrameTick";
    }
    return "Unknown";
}

}  // namespace plugin
}  // namespace ppet
