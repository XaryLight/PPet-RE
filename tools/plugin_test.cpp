// PPet - C++ 移植版
// 插件系统单元测试（需求 6）：
//   - 注册 / 启用禁用 / 事件分发 / 兴趣过滤 / 多插件接收。
// 纯逻辑、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/plugin/PluginManager.hpp>
#include <src/tools/plugin/Plugin.hpp>
#include <src/tools/core/Config.hpp>

#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using ppet::plugin::IPlugin;
using ppet::plugin::IPluginHost;
using ppet::plugin::PluginEvent;
using ppet::plugin::PluginEventKind;
using ppet::plugin::PluginManager;
using ppet::plugin::PluginPtr;

/// 测试用记录型插件：记下收到的全部事件类型与次数。
class RecorderPlugin : public IPlugin {
public:
    explicit RecorderPlugin(std::string name,
                            std::vector<PluginEventKind> interests = {})
        : name_(std::move(name)), interests_(std::move(interests)) {}

    std::string Name() const override { return name_; }
    std::vector<PluginEventKind> Interests() const override { return interests_; }
    void OnEnable(IPluginHost&) override { enabledCalls_++; }
    void OnDisable() override { disabledCalls_++; }
    void OnEvent(const PluginEvent& e) override {
        received_.push_back(e.kind);
        if (!e.source.empty()) lastSource_ = e.source;
    }

    std::vector<PluginEventKind> received_;
    std::string lastSource_;
    int enabledCalls_ = 0;
    int disabledCalls_ = 0;

private:
    std::string name_;
    std::vector<PluginEventKind> interests_;
};

/// 缺陷 2.6.3：在 OnEvent 里递归 Emit 同一事件的插件，用于验证深度上限。
class RecursiveEmitterPlugin : public IPlugin {
public:
    explicit RecursiveEmitterPlugin(IPluginHost& host, int maxRecurse)
        : host_(host), maxRecurse_(maxRecurse) {}
    std::string Name() const override { return "Rec"; }
    std::vector<PluginEventKind> Interests() const override { return {}; }  // 空=全收
    void OnEvent(const PluginEvent& e) override {
        ++calls_;
        if (recurseDepth_ < maxRecurse_) {
            ++recurseDepth_;
            host_.Emit(e);       // 重入：验证 recursive_mutex + 深度上限
            --recurseDepth_;
        }
    }
    int calls_ = 0;
private:
    IPluginHost& host_;
    int maxRecurse_;
    int recurseDepth_ = 0;
};

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    PluginManager mgr;

    // --- 注册即启用，且调用 OnEnable ---
    auto a = std::make_shared<RecorderPlugin>("A");
    mgr.Register(a);
    check(a->enabledCalls_ == 1, "注册即 OnEnable");
    check(mgr.IsEnabled("A"), "注册后默认启用");
    check(mgr.List().size() == 1 && mgr.List()[0] == "A", "List 含 A");

    // --- 事件分发给已启用插件 ---
    PluginEvent ev{PluginEventKind::ModelLoaded, "model.vrm", {}};
    mgr.Emit(ev);
    check(a->received_.size() == 1 && a->received_[0] == PluginEventKind::ModelLoaded,
          "A 收到 ModelLoaded");
    check(a->lastSource_ == "model.vrm", "事件携带 source");

    // --- 禁用后不再收到事件；OnDisable 被调用 ---
    check(mgr.SetEnabled("A", false), "SetEnabled(false) 找到 A");
    check(a->disabledCalls_ == 1, "禁用调用 OnDisable");
    mgr.Emit(ev);
    check(a->received_.size() == 1, "禁用后不再收事件");

    // --- 重新启用后恢复接收 ---
    mgr.SetEnabled("A", true);
    mgr.Emit(ev);
    check(a->received_.size() == 2, "重新启用后恢复接收");

    // --- 兴趣过滤：只收指定类型 ---
    auto b = std::make_shared<RecorderPlugin>(
        "B", std::vector<PluginEventKind>{PluginEventKind::MotionPlayed});
    mgr.Register(b);
    mgr.Emit(PluginEvent{PluginEventKind::ModelLoaded, "x", {}});
    // 注意断言顺序：必须在发出 MotionPlayed 之前检查「没收到 ModelLoaded」。
    check(b->received_.empty(), "B 不收 ModelLoaded（兴趣过滤）");
    mgr.Emit(PluginEvent{PluginEventKind::MotionPlayed, "idle", {}});
    check(b->received_.size() == 1 && b->received_[0] == PluginEventKind::MotionPlayed,
          "B 收 MotionPlayed");
    check(b->lastSource_ == "idle", "B 收到动作 id");

    // --- 多个插件同时接收 ---
    PluginEvent inter{PluginEventKind::Interaction, "", {{"region", "head"}}};
    mgr.Emit(inter);
    // A（全收、已启用）收 Interaction；B 只收 MotionPlayed，不收。
    check(a->received_.back() == PluginEventKind::Interaction, "A 收 Interaction");
    check(b->received_.size() == 1, "B 仍不收 Interaction");

    // --- 重名注册去重 ---
    auto a2 = std::make_shared<RecorderPlugin>("A");
    mgr.Register(a2);
    check(mgr.List().size() == 2, "重名替换后仍是 2 个插件");
    check(mgr.IsEnabled("A"), "新 A 默认启用");

    // --- 缺陷 2.2.10：禁用态插件被同名替换后，新实例应默认启用 ---
    mgr.SetEnabled("B", false);
    check(!mgr.IsEnabled("B"), "B 已禁用");
    auto b2 = std::make_shared<RecorderPlugin>("B");
    mgr.Register(b2);
    check(mgr.IsEnabled("B"), "禁用态插件被同名替换后新实例默认启用（缺陷 2.2.10）");
    check(b2->enabledCalls_ == 1, "新实例收到 OnEnable 回调");
    // 新实例应真的能收到事件（不再是「永久失活」）。B 只订阅 MotionPlayed。
    mgr.Emit(PluginEvent{PluginEventKind::MotionPlayed, "idle2", {}});
    check(!b2->received_.empty() &&
              b2->received_.back() == PluginEventKind::MotionPlayed,
          "替换后的新插件正常接收事件");

    // --- 找不到的插件 SetEnabled 返回 false ---
    check(!mgr.SetEnabled("Nope", true), "未知插件返回 false");

    // --- Status 快照 ---
    auto status = mgr.Status();
    check(status["A"] == true && status["B"] == true, "Status 反映启用状态");

    // --- 缺陷 2.6.3：嵌套 Emit 深度上限，不会无限递归 ---
    {
        PluginManager nested;
        auto rec = std::make_shared<RecursiveEmitterPlugin>(nested, 100);
        nested.Register(rec);
        // 请求递归 100 层，但管理器深度上限 8，应被截断，调用次数有限。
        nested.Emit(PluginEvent{PluginEventKind::AppStart, "root", {}});
        check(rec->calls_ >= 1 && rec->calls_ <= 9,
              "嵌套 Emit 被深度上限截断（不会无限递归）");
    }

    // --- 缺陷 2.6.3：Config() 注入可读 ---
    {
        PluginManager withCfg;
        ppet::AppConfig cfg;
        cfg.maxFps = 45;
        withCfg.SetConfigSource(&cfg);
        check(withCfg.Config().maxFps == 45, "Config() 返回注入的配置快照");
    }

    if (failures == 0) {
        std::printf("plugin_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("plugin_unit: %d FAILED\n", failures);
    return 1;
}
