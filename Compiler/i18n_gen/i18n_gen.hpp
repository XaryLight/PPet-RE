// PPet - C++ 移植版
// Compiler/ 文案翻译器：把 JSON / CSV 里的界面文案「翻译」成 C++ 内嵌表。
//
// 为什么要它：
//   以前中英文文案直接写在 src/core/I18n.cpp 里，两份手抄、极易不同步
//   （加一条文案要改两个 if 分支，漏改一处就出现空字符串）。
//   现在文案本体搬到 Compiler/i18n/*.json，由本工具在构建时翻译成
//   core/I18nStrings.inl（一个 constexpr 表 + 一个 X 宏），
//   运行时零成本，翻译者只需要碰 JSON。
//
// 本头文件刻意不依赖任何图形/平台设施，方便单独编译成命令行工具与单元测试。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ppet {
namespace i18n_gen {

/// 一条文案：键 + 简体中文 + 英文。
struct Entry {
    std::string key;  ///< 键，必须与 struct Strings 的成员名一致
    std::string zh;   ///< 简体中文
    std::string en;   ///< 英文
};

/// 读取一张文案表。
///
/// 支持两种格式，按扩展名自动判断：
///   - `.json`：`{"key": "text", ...}`；以 `_` 开头的键视为注释项，忽略；
///   - `.csv` ：首行表头 `key,text`，其后每行一条；支持双引号包裹与 "" 转义。
///
/// 失败时把原因写进 error 并返回 false。
bool LoadTable(const std::string& path, std::map<std::string, std::string>& out,
               std::string& error);

/// 解析 I18n.hpp，取出 `struct Strings` 的成员名（保持声明顺序）。
///
/// 有了它，翻译器就能在构建期校验「文案键 ↔ 结构体成员」一一对应：
/// 少一条、多一条、拼错一个键，都会在构建时立刻报错，而不是等到界面上出现空白。
bool ParseStructMembers(const std::string& headerPath, std::vector<std::string>& members,
                        std::string& error);

/// 校验文案表：结构体成员 ↔ 文案键必须一一对应，且译文不能为空。
bool Validate(const std::vector<std::string>& members,
              const std::map<std::string, std::string>& zh,
              const std::map<std::string, std::string>& en, std::string& error);

/// 生成 .inl 的文本内容。
///
/// 输出包含两部分：
///   1. `PPET_I18N_ENTRIES(X)` —— X 宏，一条文案一行；
///   2. `kI18nTable[]` —— 由同一个 X 宏展开出来的 constexpr 表。
/// 两者共用同一份数据，因此不可能出现「表里有、赋值时没有」的不一致。
std::string BuildInl(const std::vector<Entry>& entries, const std::string& zhSourcePath,
                     const std::string& enSourcePath);

/// 把文本转成 C++ 字符串字面量（含双引号，并转义 \ " 换行 制表符等）。
std::string EscapeCppLiteral(const std::string& text);

/// 写文件（失败时把原因写进 error）。
bool WriteFile(const std::string& path, const std::string& content, std::string& error);

/// 读文件（失败时把原因写进 error）。
bool ReadFile(const std::string& path, std::string& content, std::string& error);

}  // namespace i18n_gen
}  // namespace ppet
