// PPet - C++ 移植版
// 用系统默认程序打开外部链接（反馈页面等）。
#pragma once

#include <string>

namespace ppet {

/// 用系统默认浏览器打开 URL。
///
/// 只允许 http / https 协议：既避免误把本地文件路径当成 URL 交给系统，
/// 也避免通过 file:// 等协议绕过浏览器的安全检查。
///
/// @param url   目标链接
/// @param error 失败时写入中文原因
bool OpenExternalUrl(const std::string& url, std::string& error);

}  // namespace ppet
