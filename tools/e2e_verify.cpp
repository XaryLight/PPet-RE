// PPet - C++ 移植版
// 端到端验证：加载真实模型 -> 渲染 -> 截取画面 -> 验证全屏追踪。
//
// 这个测试回答三个问题：
//   1. PMX / VRM 真实模型能否被程序加载并渲染出可见像素？
//   2. 画面是否符合预期（模型在窗口中央、尺寸合理、不是全黑或全透明）？
//   3. 「全屏追踪」的核心换算是否正确 —— 鼠标在远处时归一化方向是否正确、
//      是否被正确限制在单位圆内？
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// GLEW 必须在 GLFW 之前引入：GLFW 会带入 gl.h，与 GLEW 冲突。
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "core/Math.hpp"
#include "model/Model.hpp"
#include "platform/GlobalCursorTracker.hpp"
#include "render/RenderUtils.hpp"

#include "stb_image_write.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

/// 统计窗口内不透明像素数。
struct PixelStats {
    int opaque = 0;
    int total = 0;
    float avgR = 0.0f, avgG = 0.0f, avgB = 0.0f;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
};

PixelStats ReadStats(int width, int height) {
    PixelStats s;
    s.total = width * height;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    bool first = true;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            if (pixels[i + 3] > 16) {
                ++s.opaque;
                sumR += pixels[i];
                sumG += pixels[i + 1];
                sumB += pixels[i + 2];
                if (first) {
                    s.minX = s.maxX = x;
                    s.minY = s.maxY = y;
                    first = false;
                } else {
                    s.minX = std::min(s.minX, x);
                    s.maxX = std::max(s.maxX, x);
                    s.minY = std::min(s.minY, y);
                    s.maxY = std::max(s.maxY, y);
                }
            }
        }
    }
    if (s.opaque > 0) {
        s.avgR = sumR / s.opaque;
        s.avgG = sumG / s.opaque;
        s.avgB = sumB / s.opaque;
    }
    return s;
}

/// 保存 PNG（用 stb_image_write 写出，便于人工检查渲染结果）。
void SavePng(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // stb_image_write 需要 top-down 行序，而 glReadPixels 是 bottom-up，需翻转。
    std::vector<unsigned char> flipped(pixels.size());
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        std::memcpy(&flipped[static_cast<std::size_t>(y) * stride],
                    &pixels[static_cast<std::size_t>(height - 1 - y) * stride], stride);
    }

    stbi_write_png(path.c_str(), width, height, 4, flipped.data(), width * 4);
}

}  // namespace

// stb_image_write 只在本文件用一次，独立实现避免与其它 TU 的 stb_image 冲突。
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "[GLFW] %d: %s\n", code, desc);
    });

    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW 初始化失败\n");
        return 2;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    const int width = 400;
    const int height = 600;
    GLFWwindow* window = glfwCreateWindow(width, height, "PPet E2E", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "创建窗口失败\n");
        glfwTerminate();
        return 2;
    }

    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK) {
        std::fprintf(stderr, "GLEW 初始化失败\n");
        glfwTerminate();
        return 2;
    }
    // glewExperimental 会遗留一个无害的 GL_INVALID_ENUM，需要清掉。
    glGetError();

    std::printf("=== PPet 端到端验证 ===\n");

    // ---------------------------------------------------------------
    // 1. 全屏追踪换算验证（不依赖图形，直接验证数学正确性）
    // ---------------------------------------------------------------
    std::printf("\n[1] 全屏追踪（全局鼠标 -> 模型朝向）换算验证\n");

    {
        ppet::Vec2 out;
        // 鼠标正好在模型中心 -> 归一化坐标应为 (0,0)。
        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(500, 500),
                                                   ppet::Vec2(500, 500), 400.0f, &out);
        Check(std::fabs(out.x) < 1e-4 && std::fabs(out.y) < 1e-4,
              "鼠标位于模型中心时归一化坐标为 (0,0)");

        // 鼠标在右侧 -> x 为正。
        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(700, 500),
                                                   ppet::Vec2(500, 500), 400.0f, &out);
        Check(out.x > 0.4f && out.y == 0.0f, "鼠标在右侧时 x 为正且 y 为 0");

        // 鼠标在下方 -> 屏幕 y 向下，模型空间 y 向上，故 y 应为负。
        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(500, 700),
                                                   ppet::Vec2(500, 500), 400.0f, &out);
        Check(out.y < -0.4f, "鼠标在屏幕下方时归一化 y 为负（模型空间 Y 向上）");

        // 鼠标远在屏幕另一端 -> 方向应被限制在单位圆内（长度 <= 1）。
        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(1900, 1000),
                                                   ppet::Vec2(500, 500), 400.0f, &out);
        const float len = std::sqrt(out.x * out.x + out.y * out.y);
        Check(len <= 1.0f + 1e-4f, "远离模型时归一化向量被限制在单位圆内");
        Check(len > 0.9f, "远离模型时方向仍保持接近单位长度（朝向不丢失）");

        // 半径为 0 的退化情况不应产生 NaN。
        ppet::GlobalCursorTracker::NormalizeAround(ppet::Vec2(100, 100),
                                                   ppet::Vec2(500, 500), 0.0f, &out);
        Check(std::isfinite(out.x) && std::isfinite(out.y), "半径为 0 时不产生 NaN");
    }

    // ---------------------------------------------------------------
    // 2. 真实模型加载与渲染
    // ---------------------------------------------------------------
    const std::string base = "tools/testdata/";
    struct ModelCase {
        const char* file;
        const char* label;
        ppet::ModelType expectedType;
    };
    const ModelCase cases[] = {
        {"sample_vrm1_twist.vrm", "VRM 1.0 (真实样本)", ppet::ModelType::Vrm},
        {"sample_vrm0_catbot.vrm", "VRM 0.x (真实样本)", ppet::ModelType::Vrm},
        {"test.pmx", "PMX (自造测试模型)", ppet::ModelType::Pmx},
    };

    for (const auto& c : cases) {
        const std::string path = base + c.file;
        std::printf("\n[模型] %s (%s)\n", c.label, c.file);

        // 类型识别
        const ppet::ModelType detected = ppet::DetectModelType(path);
        Check(detected == c.expectedType,
              std::string("文件类型识别正确 -> ") +
                  (detected == ppet::ModelType::Vrm ? "VRM" : detected == ppet::ModelType::Pmx
                                                        ? "PMX"
                                                        : "未知"));

        ppet::ModelPtr model = ppet::CreateModel(detected);
        Check(model != nullptr, "模型后端实例创建成功");
        if (!model) continue;

        std::string error;
        const bool loaded = model->Load(path, error);
        Check(loaded, loaded ? "模型加载成功：" + model->DisplayName()
                             : "模型加载失败：" + error);
        if (!loaded) continue;

        Check(model->IsLoaded(), "模型状态标记为已加载");

        // 渲染若干帧，模拟真实运行。
        ppet::RenderContext ctx;
        ctx.viewportWidth = width;
        ctx.viewportHeight = height;
        ctx.deltaTime = 1.0f / 60.0f;
        ctx.windowOrigin = ppet::Vec2(0, 0);
        ctx.windowSize = ppet::Vec2(static_cast<float>(width), static_cast<float>(height));
        ctx.modelScreenCenter = ppet::Vec2(width * 0.5f, height * 0.5f);
        ctx.modelScreenRadius = static_cast<float>(std::max(width, height)) * 0.5f;
        ctx.hasGlobalCursor = true;
        ctx.globalCursor = ppet::Vec2(1900, 100);  // 模拟鼠标在屏幕右上角
        ctx.localCursor = ppet::Vec2(0.8f, 0.4f);

        model->OnResize(width, height);

        glViewport(0, 0, width, height);
        ppet::ClearTransparent();

        for (int i = 0; i < 30; ++i) {
            model->Update(ctx);
            model->Render(ctx);
        }
        glFinish();

        const PixelStats stats = ReadStats(width, height);
        std::printf("       可见像素 %d / %d，平均色 (%.1f, %.1f, %.1f)，包围盒 x[%d..%d] y[%d..%d]\n",
                    stats.opaque, stats.total, stats.avgR, stats.avgG, stats.avgB, stats.minX,
                    stats.maxX, stats.minY, stats.maxY);

        Check(stats.opaque > 500, "模型确实渲染出了可见像素（>500）");
        Check(stats.avgR + stats.avgG + stats.avgB > 30.0f, "画面不是全黑");
        Check(stats.maxX - stats.minX > 20 && stats.maxY - stats.minY > 20,
              "模型占据的屏幕范围合理（不是一条线或一个点）");

        // 模型应大致居中：包围盒中心与窗口中心的偏差不超过窗口尺寸的一半。
        const float centerX = (static_cast<float>(stats.minX) + stats.maxX) * 0.5f;
        const float centerY = (static_cast<float>(stats.minY) + stats.maxY) * 0.5f;
        Check(std::fabs(centerX - width * 0.5f) < width * 0.45f &&
                  std::fabs(centerY - height * 0.5f) < height * 0.45f,
              "模型大致位于窗口中央");

        // 保存截图供人工检查。
        const std::string shot = std::string("build/screenshots/") + c.file + ".png";
        SavePng(shot, width, height);
        std::printf("       截图已保存: %s\n", shot.c_str());
    }

    // ---------------------------------------------------------------
    // 3. 畸形模型必须被安全拒绝
    // ---------------------------------------------------------------
    std::printf("\n[3] 畸形模型的健壮性\n");
    {
        const char* broken[] = {"broken_truncated.pmx", "broken_random.pmx",
                                "broken_header_only.pmx"};
        for (const char* f : broken) {
            auto model = ppet::CreateModel(ppet::ModelType::Pmx);
            std::string error;
            const bool ok = model->Load(base + f, error);
            Check(!ok, std::string("拒绝畸形 PMX：") + f);
        }
        const char* brokenVrm[] = {"broken_truncated.vrm", "broken_random.vrm",
                                   "broken_badjson.vrm", "broken_oob.vrm",
                                   "broken_count.vrm", "broken_numoverflow.vrm"};
        for (const char* f : brokenVrm) {
            auto model = ppet::CreateModel(ppet::ModelType::Vrm);
            std::string error;
            const bool ok = model->Load(base + f, error);
            Check(!ok, std::string("拒绝畸形 VRM：") + f);
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("\n---------------------------------------------\n");
    std::printf("检查项：%d，失败：%d\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "全部通过。" : "存在失败项。");
    return g_failures == 0 ? 0 : 1;
}
