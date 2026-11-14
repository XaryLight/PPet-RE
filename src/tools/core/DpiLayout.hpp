// PPet - C++ 移植版
// 高 DPI 布局：把「framebuffer 物理像素」与「窗口逻辑尺寸」明确分开。
//
// 【为什么需要它】
//   Windows Per-monitor V2 下，同一份配置（例如 840x600）在不同缩放的显示器上，
//   窗口客户区是屏幕坐标、而 OpenGL framebuffer 是物理像素，两者可能相差
//   1.06x / 2.13x / 更多。早期实现用 glfwGetWindowSize（逻辑尺寸）去算正交
//   投影矩阵，却没有显式设置 glViewport —— GL 默认 viewport 是 framebuffer
//   物理尺寸，于是「矩阵按 840x600 算、视口按 1789x1278 画」，
//   模型被放大/偏移，超出窗口客户区的像素直接被截断。
//
//   修复原则：渲染用的 viewportWidth/Height 必须与 glViewport 一致，
//   统一取 framebuffer 物理像素；窗口逻辑尺寸只用于配置、位置与鼠标归一化。
//
// 纯逻辑、零依赖（不 include GL / GLFW），三种 CMake 配置均可单测。
#pragma once

namespace ppet {

struct ViewportSize {
    int width = 0;
    int height = 0;
};

/// 解析本帧应使用的 glViewport / 投影矩阵尺寸。
///
/// 优先用 framebuffer 物理像素；framebuffer 无效（窗口最小化、上下文尚未
/// 就绪、无头环境）时回退窗口尺寸；两者都无效给 1x1，避免
/// glViewport(0,0) 触发 GL_INVALID_VALUE。
inline ViewportSize ResolveViewportSize(int fbWidth, int fbHeight,
                                        int windowWidth, int windowHeight) {
    int w = fbWidth > 0 ? fbWidth : windowWidth;
    int h = fbHeight > 0 ? fbHeight : windowHeight;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    return {w, h};
}

/// 由 framebuffer 与窗口宽度推算内容缩放比（仅用于日志与诊断）。
/// 无法推算时返回 1.0f。
inline float EstimateContentScale(int fbWidth, int windowWidth) {
    if (windowWidth <= 0 || fbWidth <= 0) return 1.0f;
    return static_cast<float>(fbWidth) / static_cast<float>(windowWidth);
}

}  // namespace ppet
