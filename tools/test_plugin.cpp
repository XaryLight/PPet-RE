// PPet - C++ 移植版
// 动态插件加载测试的「被测插件」：编译成 MODULE 库，导出标准 ABI 符号。
// 仅用于 dynamic_plugin_test，不参与产品构建。
#include <src/tools/plugin/Plugin.hpp>
#include <src/tools/plugin/PluginABI.hpp>

#include <string>

namespace {

class TestDynamicPlugin : public ppet::plugin::IPlugin {
public:
    std::string Name() const override { return "DynamicTestPlugin"; }
    void OnEvent(const ppet::plugin::PluginEvent&) override {}
};

}  // namespace

extern "C" int ppet_plugin_abi() {
    return ppet::plugin::kPluginAbiVersion;
}

extern "C" ppet::plugin::IPlugin* ppet_plugin_create() {
    return new TestDynamicPlugin();
}

extern "C" void ppet_plugin_destroy(ppet::plugin::IPlugin* p) {
    delete p;
}
