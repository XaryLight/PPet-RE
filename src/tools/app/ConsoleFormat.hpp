// PPet - C++ 移植版
// 控制台格式化纯函数模块（报告 2.3.1：App God Object 拆分）。
//
// 这里集中原先散落在 AppConsole.cpp 匿名命名空间里的纯字符串/配置格式化逻辑，
// 以及 debug 命令的大段 ModelDebugInfo 诊断文本渲染。共同特征：
//   - 无 App 状态、无 IO、无副作用；
//   - 输入是值或 const 引用，输出是字符串/bool/int；
//   - 因此可脱离 App 单测，也让 AppConsole.cpp 回归「命令分派 + 状态变更」本职。
//
// 注意：这些函数**只做字节串处理，刻意不碰 std::filesystem**。Windows 下
// path(char*) 构造会做 char -> wide 的 UTF-8 严格转换，模型路径一旦含非法
// 字节就会抛 `filesystem error: in __char_to_wide`，把 list 这类纯展示命令
// 整条打断。显示名只是切分字节串，不需要文件系统语义。
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <src/tools/core/Config.hpp>
#include <src/tools/model/Model.hpp>
#include <src/tools/model/ModelError.hpp>

namespace ppet {
namespace console_fmt {

/// 从路径取「文件名（去后缀）」。空路径返回 "(未加载)"。
std::string ConsoleBaseName(const std::string& path);

/// 第 i 个模型的显示名（优先用配置里用户改过的名字）。
std::string ModelNameAt(const AppConfig& cfg, std::size_t index);

/// 按路径找模型下标，找不到返回 -1。
int FindModelIndex(const AppConfig& cfg, const std::string& path);

/// 当前模型的显示名（列表里找不到时退回文件名）。
std::string CurrentModelName(const AppConfig& cfg);

/// 失败结果判定：只有失败/异常/用法错误才写日志，正常指令只回显控制台。
bool IsConsoleFailure(const std::string& result);

/// 模型类型可读名（PMX / VRM / Live2D / Unknown）。
const char* TypeName(ModelType type);

/// 严格解析下标：整串必须是十进制整数，否则返回 false。
bool ParseIndex(const std::string& text, int& out);

/// 用分隔符连接字符串。
std::string Join(const std::vector<std::string>& items, const std::string& sep);

/// 把加载错误格式化成 "fail: ..."（优先一句话原因，否则整串）。
std::string Failure(const ModelLoadError& err);

/// 分节标题 "[name]"。
std::string Section(const std::string& name);

/// debug 命令格式化所需的「App 侧状态快照」。
/// 由 ExecuteConsoleCommand 采集后传入，使格式化本身与 App 解耦、可单测。
struct DebugViewContext {
    std::string fallbackPath;  ///< cfg.modelPath：info.path/modelId 为空时回退显示
    std::vector<std::pair<std::string, std::string>> resources;  ///< RenderResources()
    std::vector<std::string> motionNames;    ///< MotionNames()
    std::vector<std::string> expressionIds;  ///< ExpressionPresetIds()
    bool hasLoadError = false;               ///< !lastLoadError_.ok()
    std::string errorCategory;               ///< lastLoadError_.CategoryName()
    std::string errorSummary;                ///< lastLoadError_.summary
    std::vector<std::string> errorDetails;   ///< lastLoadError_.details
};

/// 把 ModelDebugInfo 按 section 分节格式化成诊断文本。
/// section 大小写无关；"all" / "debuginfo" / 空 输出全部，否则只输出匹配的节。
std::string FormatDebugInfo(const ModelDebugInfo& info,
                            const std::string& section,
                            const DebugViewContext& ctx);

}  // namespace console_fmt
}  // namespace ppet
