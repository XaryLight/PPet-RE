// PPet - C++ 移植版
// 动态插件加载器（报告 2.3.3）：扫描目录加载 .so/.dll/.dylib，
// 校验 ABI 后把插件包装成 shared_ptr 交给 PluginManager::Register。
//
// 内置插件（BuiltinPlugins.hpp）仍由应用代码内注册，二者互不排斥：
// 动态插件是对扩展点的补全，让第三方无需改本项目源码即可扩展。
#pragma once

#include <src/tools/plugin/Plugin.hpp>
#include <src/tools/plugin/PluginManager.hpp>

#include <string>
#include <vector>

namespace ppet::plugin {

/// 加载单个动态插件库的结果。
struct PluginLoadResult {
    PluginPtr plugin;      ///< 成功时非空
    std::string error;     ///< 失败原因（含系统 dlerror 文本）
};

/// 加载单个动态库文件。失败时 plugin 为空、error 非空。
/// 成功返回的 PluginPtr 持有库句柄：析构时先调用插件的 destroy，再 dlclose。
PluginLoadResult LoadPluginFile(const std::string& path);

/// 扫描目录（仅顶层，非递归）加载所有受支持的动态库，注册到 manager。
/// @return 成功加载的插件名列表。失败项通过 log/回调可选上报（此处静默跳过，
///         避免一个坏插件阻断其它插件；调用方可对返回列表与目录内容做差集得知）。
std::vector<std::string> LoadPluginsFromDirectory(PluginManager& manager,
                                                  const std::string& directory);

/// 平台默认插件目录名（"plugins"），调用方与 exe 同目录拼接。
const char* DefaultPluginDirectoryName();

}  // namespace ppet::plugin
