// PPet - C++ 移植版
// 开机启动设置。
//
// 平台实现：
//   - Windows: 注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//   - macOS  : ~/Library/LaunchAgents/com.ppet.ppet.plist
//   - Linux  : ~/.config/autostart/ppet.desktop（XDG 自启动规范）
#pragma once

#include <string>

namespace ppet {

/// 查询开机启动状态。
///
/// @param exePath 输出当前已注册的启动程序路径；未注册时写入空字符串。
///                调用方可以把它与 GetExecutablePath() 的结果比较，
///                判断「已启用但指向了另一个副本」（例如用户移动了程序目录）。
/// @return 是否处于开机启动状态。
bool IsAutoStartEnabled(std::string& exePath);

/// 设置或取消开机启动。
/// @param enabled true = 开机启动；false = 取消
/// @param error   失败时写入中文原因
bool SetAutoStart(bool enabled, std::string& error);

/// 获取当前可执行文件的绝对路径。
bool GetExecutablePath(std::string& path);

}  // namespace ppet
