// PPet - C++ 移植版
// 插件管理器：注册 / 启用禁用 / 事件分发（需求 6）。
//
// 自带 IPluginHost 实现，应用侧通过 Register() 注入插件、通过 Emit() 广播事件。
// 事件只分发给「已启用」且「关心该事件类型」的插件；Interests 返回空表示全收。
#pragma once

#include <src/tools/plugin/Plugin.hpp>

#include <map>
#include <mutex>
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

    // --- 行为契约仲裁（缺陷 5） ---
    // 只统计「已启用」插件，结果供 App 每帧喂给模型状态机 / 动作源调度。

    /// 收集已启用插件请求的行为状态覆盖并仲裁：取 StatePriority 最高（且 >0）
    /// 的覆盖。无插件请求时返回 nullopt。插件间冲突只保留最强的一个，与内置
    /// 状态的最终仲裁由模型侧状态机（EffectiveState）完成。
    std::optional<BehaviorOverride> CollectBehaviorOverride() const;

    /// 收集已启用插件提供的动作池，按 category 分组、同类别内 priority 降序
    /// 稳定排序（并列时保留注册顺序）。供动作源调度消费。
    std::vector<PluginMotion> CollectMotions() const;

    /// 是否有已启用插件拦截该语义事件（InterceptEvent 返回 true）。
    /// 用于宿主在触发默认行为前询问「是否由插件接管」。
    bool AnyPluginIntercepts(const PluginEvent& event) const;

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

    /// 缺陷 2.4.3：IPluginHost::Emit 是公开 API，插件可能在工作线程回调，
    /// 需要保护 plugins_ / config_ / dispatchDepth_ 的读写。
    /// 用 recursive_mutex：插件在 OnEvent 里回调 host 的 Register/SetEnabled/Emit
    /// 是合法用法，同线程重入不能自锁。实际调用点仍多在 UI 主线程，锁开销可忽略。
    mutable std::recursive_mutex mutex_;
};

}  // namespace ppet::plugin
