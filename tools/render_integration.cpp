// PPet - C++ 移植版
// 渲染层端到端集成测试。
//
// 冒烟测试（render_smoke.cpp）验证的是渲染层自身的正确性；
// 本测试验证的是「模型后端 -> 渲染层」的整条链路：
//   PMX / VRM 解析 -> SkinnedMesh -> GPU 上传 -> 绘制 -> glReadPixels
//
// 只有当真实模型文件真的被解析、被蒙皮、被画到屏幕上时，
// 才能说明渲染层与两个后端的接口约定（材质字段、纹理索引、骨骼数量、
// 蒙皮矩阵、绕序、Y 翻转）确实是通的。
//
// 运行方式：
//   xvfb-run -a ./ppet_render_integration
//
// 退出码：0 = 全部通过；1 = 存在失败项；2 = 无图形显示环境，跳过图形检查。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <src/tools/model/Model.hpp>
#include <src/tools/model/pmx/PmxModel.hpp>
#include <src/tools/model/vrm/VrmModel.hpp>
#include <src/tools/render/RenderUtils.hpp>

namespace {

using namespace ppet;

constexpr int kWindowWidth = 256;
constexpr int kWindowHeight = 256;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (condition) {
        std::printf("  [ OK ] %s\n", description.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", description.c_str());
    }
}

void BeginFrame() {
    glViewport(0, 0, kWindowWidth, kWindowHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/// 统计帧缓冲里「不透明」的像素数量。
int CountOpaquePixels() {
    std::vector<unsigned char> buffer(static_cast<std::size_t>(kWindowWidth) * kWindowHeight * 4);
    glReadPixels(0, 0, kWindowWidth, kWindowHeight, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    int count = 0;
    for (std::size_t i = 3; i < buffer.size(); i += 4) {
        if (buffer[i] > 127) ++count;
    }
    return count;
}

/// 对一个已加载的后端做「更新 -> 绘制 -> 读回」的完整验证。
void RunModelTest(IModel& model, const std::string& label) {
    RenderContext ctx;
    ctx.viewportWidth = kWindowWidth;
    ctx.viewportHeight = kWindowHeight;
    ctx.deltaTime = 1.0f / 60.0f;

    // 先更新几帧，让待机动画与视线追踪跑起来。
    for (int i = 0; i < 3; ++i) model.Update(ctx);

    BeginFrame();
    model.Render(ctx);

    const int opaque = CountOpaquePixels();
    std::printf("         %s 不透明像素数 = %d / %d\n", label.c_str(), opaque,
                kWindowWidth * kWindowHeight);
    Check(opaque > 0, label + "：画面上真的画出了东西");
    Check(glGetError() == GL_NO_ERROR, label + "：绘制过程没有 GL 错误");
}

}  // namespace

int main() {
    std::printf("PPet 渲染层端到端集成测试\n");
    std::printf("--------------------------------------------------------------\n");

    if (!glfwInit()) {
        std::printf("glfwInit 失败：当前环境没有可用的图形显示，跳过图形相关检查。\n");
        return 2;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(kWindowWidth, kWindowHeight, "PPet Render Integration", nullptr, nullptr);
    if (window == nullptr) {
        std::printf("创建 GLFW 窗口失败：无法获取 GL 3.3 core 上下文，跳过图形相关检查。\n");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK) {
        std::printf("glewInit 失败（%d）：%s\n", static_cast<int>(glewStatus),
                    reinterpret_cast<const char*>(glewGetErrorString(glewStatus)));
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    std::printf("GL_VERSION  = %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("Renderer    = %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    std::printf("--------------------------------------------------------------\n");

    // -----------------------------------------------------------------------
    // PMX
    // -----------------------------------------------------------------------
    std::printf("[测试] PMX 完整链路\n");
    {
        pmx::PmxModel model;
        std::string error;
        const bool loaded = model.Load("tools/testdata/test.pmx", error);
        Check(loaded, loaded ? "PMX 模型加载成功" : ("PMX 加载失败：" + error));
        if (loaded) {
            Check(!model.Mesh().vertices.empty(), "PMX 解析出顶点");
            Check(!model.Mesh().indices.empty(), "PMX 解析出索引");
            Check(!model.Mesh().materials.empty(), "PMX 解析出材质");
            Check(!model.Mesh().bones.empty(), "PMX 解析出骨骼");
            Check(!model.Mesh().subMeshes.empty(), "PMX 解析出子网格");
            RunModelTest(model, "PMX");
        }
    }

    // -----------------------------------------------------------------------
    // VRM
    // -----------------------------------------------------------------------
    std::printf("[测试] VRM 完整链路\n");
    {
        vrm::VrmModel model;
        std::string error;
        const bool loaded = model.Load("tools/testdata/test.vrm", error);
        Check(loaded, loaded ? "VRM 模型加载成功" : ("VRM 加载失败：" + error));
        if (loaded) {
            Check(!model.Mesh().vertices.empty(), "VRM 解析出顶点");
            Check(!model.Mesh().indices.empty(), "VRM 解析出索引");
            Check(!model.Mesh().materials.empty(), "VRM 解析出材质");
            Check(!model.Mesh().bones.empty(), "VRM 解析出骨骼");
            Check(!model.Mesh().subMeshes.empty(), "VRM 解析出子网格");
            RunModelTest(model, "VRM");
        }
    }

    // -----------------------------------------------------------------------
    // 表情 / 形变后重新上传顶点，验证渲染层能接受二次 Upload
    // -----------------------------------------------------------------------
    std::printf("[测试] VRM 表情触发二次顶点上传\n");
    {
        vrm::VrmModel model;
        std::string error;
        if (model.Load("tools/testdata/test.vrm", error)) {
            RenderContext ctx;
            ctx.viewportWidth = kWindowWidth;
            ctx.viewportHeight = kWindowHeight;
            ctx.deltaTime = 1.0f / 60.0f;
            model.Update(ctx);
            BeginFrame();
            model.Render(ctx);

            // 设置一个表情权重，触发 morph 后的顶点重传。
            model.SetExpressionWeight("blink", 1.0f);
            model.Update(ctx);
            BeginFrame();
            model.Render(ctx);
            Check(glGetError() == GL_NO_ERROR, "表情变化后重新上传顶点并绘制没有 GL 错误");
            Check(CountOpaquePixels() > 0, "表情变化后画面仍然有内容");
        }
    }

    std::printf("--------------------------------------------------------------\n");
    std::printf("检查项：%d，失败：%d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("全部通过。\n");
    } else {
        std::printf("存在失败项。\n");
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return g_failures == 0 ? 0 : 1;
}
