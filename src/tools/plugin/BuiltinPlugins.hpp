// PPet - C++ 移植版
// 内置示例插件（需求 6 演示用）。
//
// 这些插件在 App 启动时注册，用来验证「事件总线真的把应用行为广播给了插件」。
// 它们不依赖任何 GUI / 渲染，纯逻辑 + 日志，因此也便于在单测里直接实例化。
#pragma once

#include <src/tools/plugin/Plugin.hpp>
#include <src/tools/core/Log.hpp>

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace ppet::plugin {

/// 示例插件：模型加载时打招呼（演示 ModelLoaded 事件）。
class GreeterPlugin : public IPlugin {
public:
    std::string Name() const override { return "greeter"; }

    std::vector<PluginEventKind> Interests() const override {
        return {PluginEventKind::ModelLoaded, PluginEventKind::Interaction};
    }

    void OnEvent(const PluginEvent& event) override {
        if (event.kind == PluginEventKind::ModelLoaded) {
            lastModel_ = event.source;
            PPET_LOG_TAG("PLUGIN", "[greeter] 模型已加载：%s", event.source.c_str());
        } else if (event.kind == PluginEventKind::Interaction) {
            PPET_LOG_TAG("PLUGIN", "[greeter] 用户互动：%s",
                         event.payload.count("region") ? event.payload.at("region").c_str() : "?");
        }
    }

    /// 最近一次加载的模型路径（诊断 / 单测用）。
    const std::string& LastModel() const { return lastModel_; }

private:
    std::string lastModel_;
};

/// 示例插件：统计收到的各类事件数（演示兴趣过滤 + 计数）。
class CounterPlugin : public IPlugin {
public:
    std::string Name() const override { return "counter"; }

    // 故意只关心「动作」相关事件，验证兴趣过滤。
    std::vector<PluginEventKind> Interests() const override {
        return {PluginEventKind::MotionPlayed, PluginEventKind::MotionStopped};
    }

    void OnEvent(const PluginEvent& event) override {
        counts_[PluginEventKindName(event.kind)]++;
        if (event.kind == PluginEventKind::MotionPlayed && !event.source.empty()) {
            byId_[event.source]++;
        }
    }

    int Count(const std::string& kindName) const {
        auto it = counts_.find(kindName);
        return it == counts_.end() ? 0 : it->second;
    }

    int CountForId(const std::string& id) const {
        auto it = byId_.find(id);
        return it == byId_.end() ? 0 : it->second;
    }

private:
    std::map<std::string, int> counts_;
    std::map<std::string, int> byId_;
};

}  // namespace ppet::plugin
