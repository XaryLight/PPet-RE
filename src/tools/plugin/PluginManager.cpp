// PPet - C++ 移植版
// 插件管理器实现（需求 6）。
#include <src/tools/plugin/PluginManager.hpp>

#include <src/tools/core/Config.hpp>

#include <algorithm>

namespace ppet::plugin {

bool PluginManager::InterestedIn(const IPlugin& plugin, PluginEventKind kind) {
    const std::vector<PluginEventKind> interests = plugin.Interests();
    if (interests.empty()) return true;  // 空兴趣 = 接收全部
    return std::find(interests.begin(), interests.end(), kind) != interests.end();
}

void PluginManager::Register(PluginPtr plugin) {
    if (!plugin) return;
    // 按名字去重：同名插件用新实例替换旧实例。
    for (auto& e : plugins_) {
        if (e.plugin && e.plugin->Name() == plugin->Name()) {
            if (e.enabled) {
                e.plugin->OnDisable();
                plugin->OnEnable(*this);
            }
            e.plugin = plugin;
            return;
        }
    }
    plugin->OnEnable(*this);
    plugins_.push_back({plugin, true});
}

bool PluginManager::SetEnabled(const std::string& name, bool enabled) {
    for (auto& e : plugins_) {
        if (e.plugin && e.plugin->Name() == name) {
            if (e.enabled != enabled) {
                e.enabled = enabled;
                if (enabled) e.plugin->OnEnable(*this);
                else e.plugin->OnDisable();
            }
            return true;
        }
    }
    return false;
}

bool PluginManager::IsEnabled(const std::string& name) const {
    for (const auto& e : plugins_) {
        if (e.plugin && e.plugin->Name() == name) return e.enabled;
    }
    return false;
}

std::vector<std::string> PluginManager::List() const {
    std::vector<std::string> out;
    out.reserve(plugins_.size());
    for (const auto& e : plugins_) {
        if (e.plugin) out.push_back(e.plugin->Name());
    }
    return out;
}

std::map<std::string, bool> PluginManager::Status() const {
    std::map<std::string, bool> out;
    for (const auto& e : plugins_) {
        if (e.plugin) out[e.plugin->Name()] = e.enabled;
    }
    return out;
}

void PluginManager::Emit(const PluginEvent& event) {
    // 插件在 OnEvent 里注册新插件时不应破坏当前遍历：先对插件列表做一份快照。
    // 同时限制嵌套深度，避免插件互相触发事件造成无限递归。
    if (dispatchDepth_ >= kMaxDispatchDepth) return;
    ++dispatchDepth_;

    // 快照（按值拷贝，避免底层 vector 重分配导致迭代器失效）。
    const std::vector<PluginPtr> snapshot = [&] {
        std::vector<PluginPtr> out;
        out.reserve(plugins_.size());
        for (const auto& e : plugins_) {
            if (e.plugin) out.push_back(e.plugin);
        }
        return out;
    }();

    for (const PluginPtr& plugin : snapshot) {
        if (!plugin) continue;
        // 快照之后插件可能已被禁用，逐个再查一次启用状态。
        bool enabled = false;
        for (const auto& e : plugins_) {
            if (e.plugin == plugin) { enabled = e.enabled; break; }
        }
        if (!enabled) continue;
        if (InterestedIn(*plugin, event.kind)) {
            plugin->OnEvent(event);
        }
    }

    --dispatchDepth_;
}

const AppConfig& PluginManager::Config() const {
    // 未注入配置时返回一个静态默认配置，保证 Config() 永不返回空引用
    // （即便插件在配置加载前被启用，也不会崩溃）。
    static const AppConfig kDefault;
    return config_ ? *config_ : kDefault;
}

}  // namespace ppet::plugin
