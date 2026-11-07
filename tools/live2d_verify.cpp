// PPet - Live2D 渲染验证工具
//
// 用真实的 Cubism SDK（PPET_WITH_CUBISM=ON）加载一个兼容的 Live2D 模型，
// 在离屏（隐藏）GLFW 窗口中渲染若干帧，读回像素、统计非透明覆盖度，
// 并把结果合成到一张 PNG 便于人工核验渲染是否正确（居中、大小、朝向）。
//
// 用法：
//   ppet_live2d_verify <model.model3.json> [out.png]
// 不传路径时默认尝试 SDK 自带的 Hiyori 示例模型。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/Model.hpp>
#include <api/live2d/Live2DModel.hpp>

// 注意：STBI_WRITE_NO_STDIO 必须在包含头文件之前定义，
// 否则带 stdio 的接口（stbi_write_png_to_file）不会被声明。
#ifndef STBI_WRITE_NO_STDIO
#define STBI_WRITE_NO_STDIO
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

/// stbi_write_png_to_func 的回调：把数据追加到内存缓冲。
void AppendToBuffer(void* context, void* data, int size) {
    auto* buffer = static_cast<std::string*>(context);
    buffer->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

int WritePng(const char* path, int w, int h, const unsigned char* rgba) {
    stbi_flip_vertically_on_write(1);  // glReadPixels 是 bottom-up，翻转成 top-down。
    std::string buffer;
    if (!stbi_write_png_to_func(&AppendToBuffer, &buffer, w, h, 4, rgba, w * 4)) {
        return 0;
    }
    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return 0;
    const std::size_t written = std::fwrite(buffer.data(), 1, buffer.size(), fp);
    std::fclose(fp);
    return written == buffer.size() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string modelPath;
    std::string outPath = "live2d_verify.png";
    if (argc > 1) modelPath = argv[1];
    if (argc > 2) outPath = argv[2];
    if (modelPath.empty()) {
        std::fprintf(stderr,
                     "[live2d_verify] 用法：%s <model3.json> [输出png]\n"
                     "  model3.json 需为 Cubism 3+ 格式（含同名 .moc3 与贴图目录）\n",
                     argv[0]);
        return 1;
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "[live2d_verify] GLFW 初始化失败\n");
        return 2;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // 隐藏窗口即可，不需要可见。
    GLFWwindow* window = glfwCreateWindow(600, 600, "live2d_verify", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "[live2d_verify] 创建窗口失败\n");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::fprintf(stderr, "[live2d_verify] GLEW 初始化失败\n");
        return 2;
    }

    ppet::live2d::Live2DModel model;
    std::string error;
    if (!model.Load(modelPath, error)) {
        std::fprintf(stderr, "[live2d_verify] 模型加载失败：%s\n", error.c_str());
        return 1;
    }
    std::printf("[live2d_verify] 已加载模型：%s\n", model.DisplayName().c_str());

    const int W = 600, H = 600;
    glViewport(0, 0, W, H);

    ppet::RenderContext ctx;
    ctx.viewportWidth = W;
    ctx.viewportHeight = H;
    ctx.deltaTime = 1.0f / 60.0f;
    ctx.renderScale = 1.0f;

    // 渲染若干帧，让 idle 动作 / 呼吸产生一个非默认姿态。
    for (int f = 0; f < 20; ++f) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        model.Update(ctx);
        model.Render(ctx);
        ctx.deltaTime = 1.0f / 60.0f;
    }
    glFinish();

    std::vector<unsigned char> pixels(static_cast<std::size_t>(W * H * 4));
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // 统计非透明覆盖度，并把 premultiplied 结果合成到浅灰背景上便于查看。
    long covered = 0;
    std::vector<unsigned char> out(static_cast<std::size_t>(W * H * 4));
    const unsigned char bg[3] = {0x99, 0x99, 0x99};
    for (int i = 0; i < W * H; ++i) {
        const unsigned char r = pixels[static_cast<std::size_t>(i) * 4 + 0];
        const unsigned char g = pixels[static_cast<std::size_t>(i) * 4 + 1];
        const unsigned char b = pixels[static_cast<std::size_t>(i) * 4 + 2];
        const unsigned char a = pixels[static_cast<std::size_t>(i) * 4 + 3];
        if (a > 10) ++covered;
        // premultiplied over bg：out = premult + bg*(1-a)
        const float af = a / 255.0f;
        const float inv = 1.0f - af;
        out[static_cast<std::size_t>(i) * 4 + 0] =
            static_cast<unsigned char>(r + bg[0] * inv);
        out[static_cast<std::size_t>(i) * 4 + 1] =
            static_cast<unsigned char>(g + bg[1] * inv);
        out[static_cast<std::size_t>(i) * 4 + 2] =
            static_cast<unsigned char>(b + bg[2] * inv);
        out[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }

    if (!WritePng(outPath.c_str(), W, H, out.data())) {
        std::fprintf(stderr, "[live2d_verify] 保存 PNG 失败：%s\n", outPath.c_str());
        return 3;
    }

    const float ratio = static_cast<float>(covered) / static_cast<float>(W * H) * 100.0f;
    std::printf("[live2d_verify] 非透明像素：%ld / %d（覆盖 %.2f%%）\n", covered,
                W * H, ratio);
    std::printf("[live2d_verify] 预览图已保存：%s\n", outPath.c_str());

    // 覆盖度极低通常表示模型没有正确落位（例如被错误地缩小到 1/ppu）。
    if (covered < 2000) {
        std::fprintf(stderr,
                     "[live2d_verify] 警告：覆盖度过低，模型可能未正确渲染\n");
        return 4;
    }
    return 0;
}
