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

void ClearTransparent() {
    // 桌宠需要真透明背景：清成 (0,0,0,0)，
    // 配合开启了 alpha 混合的合成阶段，窗口才能透出桌面。
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

bool CheckGlError(const char* file, int line) {
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR) return false;
    std::fprintf(stderr, "[OpenGL 错误] %s:%d -> 0x%04X\n", file ? file : "?", line,
                 static_cast<unsigned>(err));
    // 把错误队列里残留的错误一次性取干净，避免后续排查时被旧错误干扰。
    while (glGetError() != GL_NO_ERROR) {
    }
    return true;
}

void PrintGlError(const char* context) {
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR) return;
    std::fprintf(stderr, "[OpenGL 错误] %s -> 0x%04X\n", context ? context : "?",
                 static_cast<unsigned>(err));
    while (glGetError() != GL_NO_ERROR) {
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
