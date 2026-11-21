// PPet - 语言检测（报告 4.1.5：--lang 覆盖 + 系统语言自动检测）
//
// 纯逻辑优先：locale 标签解析做成不依赖平台的纯函数，便于单测；
// 真正的系统探测（环境变量 / 系统 API）收敛到 DetectSystemLanguage()。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <src/tools/core/Config.hpp>

namespace ppet {

/// 解析 --lang 的值。接受 zh / zh-CN / zh_CN / en / en-US / en_US / chinese / english
/// （大小写不敏感）。无法识别返回 nullopt。
std::optional<Language> ParseLanguageOption(std::string_view value);

/// 从 locale 标签推断语言（纯函数，可单测）。
/// 识别规则：以 "zh"/"cmn"/"chinese" 开头 → Chinese；以 "en"/"english" 开头 → English；
/// 其它（含空串、C/POSIX）→ nullopt。大小写不敏感，分隔符 '-'/'_' 一视同仁。
std::optional<Language> LanguageFromLocaleTag(std::string_view tag);

/// 探测系统语言：依次看 LC_ALL / LC_MESSAGES / LANG 环境变量，再退回平台 API。
/// 全部无法判断时返回 Chinese（与项目默认一致）。
Language DetectSystemLanguage();

}  // namespace ppet
