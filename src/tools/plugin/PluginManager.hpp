// PPet - C++ 移植版
// 插件管理器：注册 / 启用禁用 / 事件分发（需求 6）。
//
// 自带 IPluginHost 实现，应用侧通过 Register() 注入插件、通过 Emit() 广播事件。
// 事件只分发给「已启用」且「关心该事件类型」的插件；Interests 返回空表示全收。
#pragma once

#include <src/tools/plugin/Plugin.hpp>

#include <map>
#include <string>
#include <vector>

namespace ppet::plugin {

/// 插件管理器：同时也是 IPluginHost 的实现。
class PluginManager : public IPluginHost {
public:
    /// 注册一个插件并默认启用（注册即调用 OnEnable）。
    /// 重名插件会被后者覆盖（按名字去重）。
    void Register(PluginPtr plugin);

    /// 启用 / 禁用某个插件（按名字）。禁用时调用 OnDisable。
    /// @return 找到并操作成功返回 true。
    bool SetEnabled(const std::string& name, bool enabled);

    /// 是否启用。
    bool IsEnabled(const std::string& name) const;

    /// 已注册插件名列表。
    std::vector<std::string> List() const;

    /// 每个插件的启用状态（name -> enabled），用于诊断面板 / 控制台。
    std::map<std::string, bool> Status() const;

    /// 设置配置快照来源（Emit / Config() 使用）。
    /// 应用侧在配置加载完成后调用一次即可；配置变化后指针仍有效（引用同一对象）。
    void SetConfigSource(const AppConfig* config) { config_ = config; }

    // --- IPluginHost ---
    /// 广播事件给所有「已启用且关心该类型」的插件。
    void Emit(const PluginEvent& event) override;

    /// 当前配置（引用由 SetConfigSource 注入的对象）。
    const AppConfig& Config() const override;

private:
    struct Entry {
        PluginPtr plugin;
        bool enabled = true;
    };

    static bool InterestedIn(const IPlugin& plugin, PluginEventKind kind);

    std::vector<Entry> plugins_;
    const AppConfig* config_ = nullptr;

    /// 分发嵌套深度（防止插件在回调里 Emit 造成无限递归）。
    int dispatchDepth_ = 0;
    static constexpr int kMaxDispatchDepth = 8;
};

}  // namespace ppet::plugin
