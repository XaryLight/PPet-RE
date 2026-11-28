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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 按名字去重：同名插件用新实例替换旧实例。
    for (auto& e : plugins_) {
        if (e.plugin && e.plugin->Name() == plugin->Name()) {
            // 缺陷 2.2.10：旧插件若为禁用态，早期代码既不置 enabled=true 也不
            // 调 OnEnable，新插件永久失活。语义应与首次注册一致——新实例默认启用。
            if (e.enabled) e.plugin->OnDisable();
            e.plugin = plugin;
            e.enabled = true;
            plugin->OnEnable(*this);
            return;
        }
    }
    plugin->OnEnable(*this);
    plugins_.push_back({plugin, true});
}

bool PluginManager::SetEnabled(const std::string& name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& e : plugins_) {
        if (e.plugin && e.plugin->Name() == name) return e.enabled;
    }
    return false;
}

std::vector<std::string> PluginManager::List() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(plugins_.size());
    for (const auto& e : plugins_) {
        if (e.plugin) out.push_back(e.plugin->Name());
    }
    return out;
}

std::map<std::string, bool> PluginManager::Status() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::map<std::string, bool> out;
    for (const auto& e : plugins_) {
        if (e.plugin) out[e.plugin->Name()] = e.enabled;
    }
    return out;
}

void PluginManager::Emit(const PluginEvent& event) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 插件在 OnEvent 里注册新插件时不应破坏当前遍历：先对插件列表做一份快照。
    // 同时限制嵌套深度，避免插件互相触发事件造成无限递归。
    if (dispatchDepth_ >= kMaxDispatchDepth) return;

    // RAII 深度守卫：即便插件回调抛异常，深度也会在本函数退出时恢复，
    // 否则一次异常就会让后续所有事件被误判为「递归过深」而静默丢弃。
    struct DepthGuard {
        explicit DepthGuard(int& depth) : depth_(depth) { ++depth_; }
        ~DepthGuard() { --depth_; }
        int& depth_;
    } guard(dispatchDepth_);

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
}

const AppConfig& PluginManager::Config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 未注入配置时返回一个静态默认配置，保证 Config() 永不返回空引用
    // （即便插件在配置加载前被启用，也不会崩溃）。
    static const AppConfig kDefault;
    return config_ ? *config_ : kDefault;
}

std::optional<BehaviorOverride> PluginManager::CollectBehaviorOverride() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::optional<BehaviorOverride> best;
    for (const auto& e : plugins_) {
        if (!e.enabled || !e.plugin) continue;
        const int priority = e.plugin->StatePriority();
        if (priority <= 0) continue;
        const auto state = e.plugin->WantOverrideState();
        if (!state.has_value()) continue;
        // 多个插件请求冲突时，只保留优先级最高者；并列时先注册者胜出。
        if (!best.has_value() || priority > best->priority) {
            best = BehaviorOverride{*state, priority};
        }
    }
    return best;
}

std::vector<PluginMotion> PluginManager::CollectMotions() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<PluginMotion> out;
    for (const auto& e : plugins_) {
        if (!e.enabled || !e.plugin) continue;
        std::vector<PluginMotion> motions = e.plugin->ProvideMotions();
        for (PluginMotion& m : motions) out.push_back(std::move(m));
    }
    // 按类别分组（枚举升序），同类别内按优先级降序；stable_sort 保证并列优先级
    // 保留收集（注册）顺序，结果可复现。
    std::stable_sort(out.begin(), out.end(), [](const PluginMotion& a, const PluginMotion& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.priority > b.priority;
    });
    return out;
}

bool PluginManager::AnyPluginIntercepts(const PluginEvent& event) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& e : plugins_) {
        if (!e.enabled || !e.plugin) continue;
        if (e.plugin->InterceptEvent(event)) return true;
    }
    return false;
}

}  // namespace ppet::plugin
