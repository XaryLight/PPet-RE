// PPet - C++ 移植版
// 渲染帧抓图实现。
//
// 说明：
//   - 只读取默认帧缓冲（GL_READ_FRAMEBUFFER 绑定为 0 时即 back buffer），
//     调用点位于 model_->Render() 之后、SwapBuffers() 之前；
//   - glReadPixels 返回的是「左下原点」的行序，PNG 需要「左上原点」，
//     所以按行翻转一次；
//   - App 先调用 ConsumeGlErrors() 采集本帧真正的 GL 错误，然后才抓帧；
//     抓帧自身（glReadPixels 等）可能产生的错误在末尾就地清掉，
//     不让它污染下一帧的诊断统计。
#include <src/tools/render/FrameCapture.hpp>

#include <cstring>
#include <vector>

#include <GL/glew.h>

// stb_image_write 是 header-only，实现宏在**本翻译单元**定义一次即可。
// ppet 主程序里只有这个文件包含它，其它工具（e2e_verify / live2d_verify）
// 是各自独立的可执行文件，互不冲突。
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
// stb 的实现里有多处 `{0}` 聚合初始化，在 -Wall -Wextra 下会产生
// -Wmissing-field-initializers。第三方代码不参与本项目的严格告警，
// 这里只对这一个头文件局部关闭，本文件自身代码仍受完整告警保护。
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb/stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace ppet {

bool CaptureRenderFrame(const std::string& pngPath, int& outWidth, int& outHeight,
                        std::string& error) {
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int w = viewport[2];
    const int h = viewport[3];
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        error = "视口无效，无法抓帧";
        return false;
    }

    std::vector<unsigned char> raw(static_cast<std::size_t>(w) * h * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    // 清掉抓帧本身可能产生的 GL 错误，避免下一帧被误判为渲染异常。
    while (glGetError() != GL_NO_ERROR) {
    }

    std::vector<unsigned char> flipped(raw.size(), 0);
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * stride,
                    raw.data() + static_cast<std::size_t>(h - 1 - y) * stride, stride);
    }

    if (stbi_write_png(pngPath.c_str(), w, h, 4, flipped.data(),
                       static_cast<int>(stride)) == 0) {
        error = "写 PNG 失败：" + pngPath;
        return false;
    }
    outWidth = w;
    outHeight = h;
    error.clear();
    return true;
}

}  // namespace ppet
