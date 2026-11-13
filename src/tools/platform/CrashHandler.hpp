// PPet - C++ 移植版
// 崩溃诊断：安装顶层异常过滤器，把异常码 / 地址 / 所属模块写入 last.log。
//
// 背景：Windows 上「游戏加加 GamePP / RTSS / MSI Afterburner / NVIDIA
// GeForce Experience Overlay / Discord Overlay / 录屏与帧率监控软件」会向
// 带 OpenGL / D3D 上下文的进程注入 IPC overlay DLL。这些 DLL 在受限环境下
// CreateSemaphore / CreateFileMapping 失败（错误码 5 = ERROR_ACCESS_DENIED）
// 后仍继续使用空句柄，最终以 0xC0000005 崩溃；它们的 stderr 输出会混进
// 控制台，看起来像是「PPet 自己打印的错误」。
//
// 本模块不拦截、不修复第三方 DLL，只在未处理异常发生时留下可判定的证据：
// 异常地址属于 ppet.exe 还是某个注入 DLL，并给出对应的排查建议。
#pragma once

#include <string_view>

namespace ppet::crash {

/// 崩溃来源归类（纯逻辑，可跨平台单测）。
enum class Origin { kSelf, kThirdParty, kUnknown };

/// 判断异常地址所在模块是不是 PPet 主程序。
/// 只比较文件名部分（大小写不敏感），兼容 '/' 与 '\\' 两种分隔符。
/// modulePath 或 exePath 为空时返回 kUnknown，不误判为第三方。
Origin ClassifyOrigin(std::string_view modulePath, std::string_view exePath);

/// 常见异常码 → 可读名称（未知码返回 "UNKNOWN"）。
std::string_view ExceptionName(unsigned long code);

/// 安装顶层异常过滤器（仅 Windows 有实际实现；其它平台为空操作）。
/// 必须在 Logger::Init 之后调用，崩溃信息才会追加到同一个 last.log。
void Install();

}  // namespace ppet::crash
