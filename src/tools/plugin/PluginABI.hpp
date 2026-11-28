// PPet - C++ 移植版
// 动态插件 ABI 约定（报告 2.3.3）。
//
// 一个动态插件（.so / .dll / .dylib）只需导出下面三个 C 链接符号：
//   - ppet_plugin_abi()        -> 返回 ABI 版本整数，必须与宿主常量相等；
//   - ppet_plugin_create()     -> 创建一个 IPlugin 实例（宿主用 shared_ptr 托管）；
//   - ppet_plugin_destroy(ip)  -> 销毁 ppet_plugin_create 创建的实例。
//
// 【ABI 风险] C++ 对象跨动态库边界有编译器/STL ABI 依赖，因此：
//   1) ABI 版本必须严格匹配；
//   2) 插件必须用与宿主相同的编译器与 C++ 标准构建（文档约定）。
// 宿主在 dlopen 后先校验版本，不匹配则拒绝加载并 dlclose，绝不会调用不兼容的
// create/destroy。接口本身是纯虚类，不传递 STL 容器所有权（只在同侧使用）。
#pragma once

#include <src/tools/plugin/Plugin.hpp>

namespace ppet::plugin {

/// 当前插件 ABI 版本。任何 IPlugin 虚表/签名变化都要 +1。
///
/// 版本历史：
///   - 1：初始版本（Name/Interests/OnEnable/OnDisable/OnEvent）。
///   - 2：IPlugin 末尾追加行为扩展契约（WantOverrideState/StatePriority/
///        ProvideMotions/InterceptEvent，均默认空实现）。追加在末尾保证旧虚函数
///        的 vtable 槽位偏移不变，但宿主会调用新虚方法，旧插件无法提供，故必须
///        +1 让旧动态插件在加载时被明确拒绝，而不是越界调用。
constexpr int kPluginAbiVersion = 2;

/// C 链接符号名（跨平台一致，便于文档与工具约定）。
inline constexpr char kSymbolAbiVersion[] = "ppet_plugin_abi";
inline constexpr char kSymbolCreate[] = "ppet_plugin_create";
inline constexpr char kSymbolDestroy[] = "ppet_plugin_destroy";

// 函数指针类型。extern "C" 只影响链接与符号名，调用约定默认即可。
using AbiVersionFn = int (*)();
using CreatePluginFn = IPlugin* (*)();
using DestroyPluginFn = void (*)(IPlugin*);

}  // namespace ppet::plugin
