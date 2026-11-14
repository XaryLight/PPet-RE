// PPet - C++ 移植版
// 渲染层辅助工具：色彩空间转换、清屏、GL 错误检查。
#pragma once

#include <string>

#include <src/tools/core/Math.hpp>

namespace ppet {

/// sRGB -> 线性空间（逐通道）。
/// 为什么要转换：GPU 在 sRGB 内部格式（GL_SRGB8_ALPHA8）下采样时会自动做
/// 「sRGB -> 线性」的解码，所以着色器里的颜色运算必须在线性空间进行；
/// 而 CPU 侧算出来的颜色（例如来自配置文件的十六进制色）通常是 sRGB 值。
Vec3 SrgbToLinear(const Vec3& c);

/// 线性空间 -> sRGB。
Vec3 LinearToSrgb(const Vec3& c);

/// 清空颜色缓冲与深度缓冲，颜色清成全透明（alpha = 0）。
/// 桌宠窗口需要真透明，因此不能用黑色清屏。
void ClearTransparent();

/// 每帧把 glViewport 显式设为 **framebuffer 物理像素**尺寸。
///
/// HiDPI（Windows Per-monitor V2、x2.13 等）下 framebuffer 与窗口逻辑尺寸
/// 不同；早期主循环从不设置 glViewport，GL 默认视口是创建时的 framebuffer
/// 尺寸，而投影矩阵却按 glfwGetWindowSize 的逻辑尺寸计算，两者不一致会让
/// 模型被放大/偏移、超出窗口客户区的像素被截断。
/// 渲染上下文的 viewportWidth/Height 也取同一组物理像素，保持严格一致。
void SetViewport(int width, int height);

/// 检查 OpenGL 错误队列。
/// @return 有错误时返回 true（并把错误码与调用位置打印到 stderr）。
bool CheckGlError(const char* file, int line);

/// 打印 OpenGL 错误但不改变程序流程（供调试使用）。
void PrintGlError(const char* context);

/// 清空 OpenGL 错误队列并返回最后一个错误码（0 = GL_NO_ERROR）。
/// 供调试面板判定「本帧是否正常渲染」；不打印日志，也不会干扰正常渲染路径。
/// 最多消费 32 个错误，避免驱动异常时死循环。
unsigned int ConsumeGlErrors();

}  // namespace ppet

/// 错误检查宏：出错时打印文件与行号。
/// 注意：它只报告不中断——桌面宠物运行中即使出现一次 GL 错误，
/// 也应该继续跑完这一帧，而不是让整个程序崩溃。
#define PPET_GL_CHECK() ::ppet::CheckGlError(__FILE__, __LINE__)
