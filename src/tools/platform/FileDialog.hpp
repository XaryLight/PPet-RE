// PPet - C++ 移植版
// 文件选择对话框（选择模型文件）。
//
// 平台实现：
//   - Windows: GetOpenFileNameW（系统通用对话框，无需 COM 初始化）
//   - macOS  : NSOpenPanel
//   - Linux  : 依次尝试 zenity / kdialog 命令行工具；
//               都不可用时返回失败，由上层退化为「手写路径输入」。
#pragma once

#include <string>

namespace ppet {

/// 打开「选择文件」对话框。
///
/// @param title        对话框标题
/// @param filterName   过滤器名称（显示用，例如「模型配置文件」）
/// @param patterns     过滤器模式列表，用分号分隔（例如 "*.pmx;*.vrm;*.json"）
/// @param defaultPath  默认打开的目录（可为空）
/// @param nativeOwner  父窗口句柄（可为 nullptr；平台相关：
///                     Windows 为 HWND，macOS 为 NSWindow*）
/// @param outPath      选中的文件路径（用户取消时保持不变）
/// @param error        失败时写入中文原因
/// @return 用户选择文件返回 true；取消或失败返回 false
bool OpenFileDialog(const std::string& title, const std::string& filterName,
                    const std::string& patterns, const std::string& defaultPath,
                    void* nativeOwner, std::string& outPath, std::string& error);

}  // namespace ppet
