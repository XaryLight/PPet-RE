// PPet - C++ 移植版
// 插件行为契约单元测试（缺陷 5）：
//   - IPlugin 行为扩展接口的默认实现（向后兼容）；
//   - PluginManager 的状态覆盖仲裁 / 动作池收集排序 / 事件拦截；
//   - ABI 版本常量。
// 纯逻辑、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/plugin/PluginManager.hpp>
#include <src/tools/plugin/PluginABI.hpp>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using ppet::model::BehaviorState;
using ppet::plugin::BehaviorOverride;
using ppet::plugin::IPlugin;
using ppet::plugin::MotionCategory;
using ppet::plugin::PluginEvent;
using ppet::plugin::PluginEventKind;
using ppet::plugin::PluginManager;
using ppet::plugin::PluginMotion;

/// 可配置的行为插件：四个行为扩展能力都可按需返回。
class BehaviorPlugin : public IPlugin {
public:
    explicit BehaviorPlugin(std::string name) : name_(std::move(name)) {}

    std::string Name() const override { return name_; }

    std::optional<BehaviorState> WantOverrideState() const override { return override_; }
    int StatePriority() const override { return priority_; }
    std::vector<PluginMotion> ProvideMotions() const override { return motions_; }
    bool InterceptEvent(const PluginEvent& e) override {
        intercepted_.push_back(e);
        return intercept_;
    }

    std::optional<BehaviorState> override_ = std::nullopt;
    int priority_ = 0;
    std::vector<PluginMotion> motions_;
    bool intercept_ = false;
    std::vector<PluginEvent> intercepted_;

private:
    std::string name_;
};

/// 只实现基础接口的插件（不覆写任何行为扩展），验证默认实现向后兼容。
class PlainPlugin : public IPlugin {
public:
    std::string Name() const override { return "plain"; }
};

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("FAIL: %s\n", msg); ++failures; }
    };

    // --- ABI 版本常量：本次 IPlugin 虚表追加了 4 个虚方法，版本应为 2 ---
    check(ppet::plugin::kPluginAbiVersion == 2, "插件 ABI 版本已提升到 2");

    // --- 默认实现向后兼容：不覆写行为扩展的插件返回空能力 ---
    {
        PlainPlugin plain;
        check(!plain.WantOverrideState().has_value(), "默认不请求状态覆盖");
        check(plain.StatePriority() == 0, "默认覆盖优先级为 0");
        check(plain.ProvideMotions().empty(), "默认不提供动作池");
        check(!plain.InterceptEvent(PluginEvent{}), "默认不拦截事件");
    }

    PluginManager mgr;

    // --- 无插件时：无覆盖、无动作、无拦截 ---
    check(!mgr.CollectBehaviorOverride().has_value(), "无插件时无状态覆盖");
    check(mgr.CollectMotions().empty(), "无插件时动作池为空");
    check(!mgr.AnyPluginIntercepts(PluginEvent{}), "无插件时不拦截");

    // --- 单个插件请求状态覆盖 ---
    auto focus = std::make_shared<BehaviorPlugin>("focus");
    focus->override_ = BehaviorState::Suspended;
    focus->priority_ = 50;
    mgr.Register(focus);
    {
        const auto ov = mgr.CollectBehaviorOverride();
        check(ov.has_value(), "有插件请求时返回覆盖");
        check(ov->state == BehaviorState::Suspended, "覆盖状态为 Suspended");
        check(ov->priority == 50, "覆盖优先级透传");
    }

    // --- 多插件冲突：取优先级最高者 ---
    auto quiet = std::make_shared<BehaviorPlugin>("quiet");
    quiet->override_ = BehaviorState::Talking;
    quiet->priority_ = 10;
    mgr.Register(quiet);
    {
        const auto ov = mgr.CollectBehaviorOverride();
        check(ov.has_value() && ov->priority == 50 &&
                  ov->state == BehaviorState::Suspended,
              "多插件冲突取最高优先级（focus/50 胜过 quiet/10）");
    }

    // --- StatePriority <= 0 不参与覆盖 ---
    auto weak = std::make_shared<BehaviorPlugin>("weak");
    weak->override_ = BehaviorState::Sitting;
    weak->priority_ = 0;
    mgr.Register(weak);
    {
        const auto ov = mgr.CollectBehaviorOverride();
        check(ov.has_value() && ov->priority == 50, "StatePriority<=0 不参与仲裁");
    }

    // --- 禁用插件不参与仲裁 ---
    check(mgr.SetEnabled("focus", false), "禁用 focus");
    {
        const auto ov = mgr.CollectBehaviorOverride();
        check(ov.has_value() && ov->state == BehaviorState::Talking &&
                  ov->priority == 10,
              "禁用后由次高优先级 quiet 接管");
    }

    // --- 动作池收集与排序 ---
    auto anim = std::make_shared<BehaviorPlugin>("anim");
    anim->motions_ = {
        {MotionCategory::Idle, 1, "idle_low", false},
        {MotionCategory::Touch, 9, "touch_hi", false},
        {MotionCategory::Idle, 5, "idle_hi", true},
        {MotionCategory::Touch, 3, "touch_lo", false},
    };
    mgr.Register(anim);
    {
        const auto motions = mgr.CollectMotions();
        check(motions.size() == 4, "动作池收集 4 个动作");
        // 排序：Idle 在 Touch 前；Idle 内 priority 降序；Touch 内 priority 降序。
        check(motions[0].id == "idle_hi" && motions[0].category == MotionCategory::Idle,
              "动作池按类别分组：Idle 优先");
        check(motions[1].id == "idle_low", "Idle 内优先级降序");
        check(motions[2].id == "touch_hi" && motions[2].category == MotionCategory::Touch,
              "Touch 类紧随其后");
        check(motions[3].id == "touch_lo", "Touch 内优先级降序");
    }

    // --- 事件拦截 ---
    PluginEvent headEvent;
    headEvent.kind = PluginEventKind::Interaction;
    headEvent.payload["region"] = "head";
    check(!mgr.AnyPluginIntercepts(headEvent), "无插件拦截时返回 false");

    auto guard = std::make_shared<BehaviorPlugin>("guard");
    guard->intercept_ = true;
    mgr.Register(guard);
    check(mgr.AnyPluginIntercepts(headEvent), "有插件拦截时返回 true");
    check(guard->intercepted_.size() == 1 &&
              guard->intercepted_[0].payload.at("region") == "head",
          "拦截回调收到完整事件");

    // 禁用后不再拦截
    mgr.SetEnabled("guard", false);
    check(!mgr.AnyPluginIntercepts(headEvent), "禁用后不再拦截");

    if (failures == 0) {
        std::printf("plugin_behavior_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("plugin_behavior_unit: %d FAILED\n", failures);
    return 1;
}
