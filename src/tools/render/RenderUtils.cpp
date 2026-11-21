// PPet - C++ 移植版
// 渲染层辅助工具实现。
#include <src/tools/render/RenderUtils.hpp>

#include <cmath>
#include <cstdio>

#include <GL/glew.h>

namespace ppet {
namespace {

/// sRGB 标准曲线的分段阈值（IEC 61966-2-1）。
constexpr float kSrgbToLinearThreshold = 0.04045f;
constexpr float kLinearToSrgbThreshold = 0.0031308f;

float SrgbChannelToLinear(float v) {
    if (v <= kSrgbToLinearThreshold) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float LinearChannelToSrgb(float v) {
    if (v <= kLinearToSrgbThreshold) return v * 12.92f;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

Vec3 SrgbToLinear(const Vec3& c) {
    return {SrgbChannelToLinear(c.x), SrgbChannelToLinear(c.y), SrgbChannelToLinear(c.z)};
}

Vec3 LinearToSrgb(const Vec3& c) {
    return {LinearChannelToSrgb(c.x), LinearChannelToSrgb(c.y), LinearChannelToSrgb(c.z)};
}

void SetViewport(int width, int height) {
    // 调用方（App::BuildRenderContext）已通过 ResolveViewportSize 保证 >= 1，
    // 这里再兜底一次，防止其它调用路径传入 0 触发 GL_INVALID_VALUE。
    glViewport(0, 0, width > 0 ? width : 1, height > 0 ? height : 1);
}

void ClearTransparent() {
    // glClear 受 colorMask / depthMask 影响。绘制半透明材质时
    // SkinnedMeshRenderer::ApplyMaterial 会把 glDepthMask 置成 GL_FALSE
    // （避免透明部件挡住后面的部件），这个状态会一直留到下一帧。
    // 若不在清屏前复位，glClear(GL_DEPTH_BUFFER_BIT) 等于没执行：
    // 深度缓冲残留，下一帧所有片元因深度相等被 GL_LESS 剔除 ——
    // 表现是 draw calls / FPS / GL 错误全部正常，画面却全空。
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    // 桌宠需要真透明背景：清成 (0,0,0,0)，
    // 配合开启了 alpha 混合的合成阶段，窗口才能透出桌面。
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

bool CheckGlError(const char* file, int line) {
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR) return false;
    std::fprintf(stderr, "[OpenGL 错误] %s:%d -> 0x%04X\n", file ? file : "?", line,
                 static_cast<unsigned>(err));
    // 把错误队列里残留的错误取干净，但必须有上限（缺陷 2.5.2）：
    // 驱动异常持续报错时无界 while 会死循环卡死主线程。复用 32 次上限。
    for (int guard = 0; guard < 32 && glGetError() != GL_NO_ERROR; ++guard) {
    }
    return true;
}

void PrintGlError(const char* context) {
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR) return;
    std::fprintf(stderr, "[OpenGL 错误] %s -> 0x%04X\n", context ? context : "?",
                 static_cast<unsigned>(err));
    for (int guard = 0; guard < 32 && glGetError() != GL_NO_ERROR; ++guard) {
    }
}

unsigned int ConsumeGlErrors() {
    GLenum last = GL_NO_ERROR;
    for (int guard = 0; guard < 32; ++guard) {
        const GLenum err = glGetError();
        if (err == GL_NO_ERROR) break;
        last = err;
    }
    return static_cast<unsigned int>(last);
}

}  // namespace ppet
