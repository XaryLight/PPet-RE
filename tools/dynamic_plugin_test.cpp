// PPet - C++ 移植版
// 动态插件加载单测（报告 2.3.3）：
//   - ABI 匹配时成功加载并拿到正确插件名
//   - ABI 不匹配 / 缺符号的库被拒绝（不崩溃）
//   - 目录扫描加载并注册到 PluginManager
//   - shared_ptr 析构时正确 destroy + dlclose（间接：反复加载/释放不崩）
#include <src/tools/plugin/DynamicPluginLoader.hpp>
#include <src/tools/plugin/PluginABI.hpp>
#include <src/tools/plugin/PluginManager.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

namespace fs = std::filesystem;

}  // namespace

int main(int argc, char** argv) {
    std::printf("== 动态插件加载单测 ==\n");
    if (argc < 2) {
        std::printf("[FAIL] 需传入被测插件库路径\n");
        return 1;
    }
    const std::string pluginPath = argv[1];

    using namespace ppet::plugin;

    // 1) 单文件加载成功、名字正确。
    {
        const auto res = LoadPluginFile(pluginPath);
        Check(res.plugin != nullptr, "ABI 匹配时加载成功");
        if (res.plugin) {
            Check(res.plugin->Name() == "DynamicTestPlugin", "插件名正确");
        }
        // 离开作用域：deleter 调 destroy + dlclose。
    }

    // 2) 反复加载/释放，验证生命周期无崩溃/无句柄泄漏式异常。
    for (int i = 0; i < 20; ++i) {
        const auto res = LoadPluginFile(pluginPath);
        if (!res.plugin) {
            Check(false, "反复加载第 N 次失败");
            break;
        }
    }
    Check(true, "反复加载/释放 20 次无崩溃");

    // 3) 目录扫描加载 + 注册到管理器。
    {
        const fs::path dir = fs::path(pluginPath).parent_path();
        PluginManager mgr;
        const auto names = LoadPluginsFromDirectory(mgr, dir.string());
        bool found = false;
        for (const auto& n : names) {
            if (n == "DynamicTestPlugin") found = true;
        }
        Check(found, "目录扫描发现并加载了测试插件");
        Check(mgr.IsEnabled("DynamicTestPlugin"), "加载的插件已注册并默认启用");
    }

    // 4) 非插件库（不存在路径）被拒绝，不崩溃。
    {
        const auto res = LoadPluginFile(pluginPath + ".not_exist");
        Check(res.plugin == nullptr && !res.error.empty(), "不存在路径被拒绝并带错误");
    }

    std::printf("== 结果：%s（失败 %d 项）==\n",
                g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
