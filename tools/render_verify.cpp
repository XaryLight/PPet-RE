// PPet - C++ 移植版
// 渲染验证工具（T1）：加载一个模型 -> 渲染若干帧 -> 截图 -> 输出 JSON 诊断报告。
//
// 【它回答什么问题】
//   「这个模型加上去之后，到底有没有正常渲染出来？」——不是靠肉眼看黑盒窗口，
//   而是用 glReadPixels 统计非透明像素覆盖率、颜色多样性、GL 错误，
//   再结合模型自身的诊断信息（骨架/贴图/缺失资源）给出可判定的结论。
//
// 【支持的输入】
//   .vrm / .pmx / .zip（PMX 模型包）/ .model3.json / .moc3 / 模型目录
//
// 【用法】
//   ppet_render_verify --model <path> [--shot out.png] [--report out.json]
//                      [--width 480] [--height 640] [--frames 30]
//                      [--min-coverage 0.2]
//
// 退出码：0 = PASS 或 WARN（可出图）；1 = FAIL（加载/渲染失败）；2 = 环境错误。
// JSON 报告交给 CI 或 tools/windows_verify.ps1 汇总，PNG 供人工核验。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <api/vendor/Json.hpp>
#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/model/Model.hpp>
#include <src/tools/model/ModelError.hpp>
#include <src/tools/render/RenderUtils.hpp>

// stb_image_write 的实现宏只在本可执行文件里定义一次。
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb/stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

struct Options {
    std::string model;
    std::string shot = "ppet_verify.png";
    std::string report = "ppet_verify.json";
    int width = 480;
    int height = 640;
    int frames = 30;
    double minCoverage = 0.2;  // 非透明像素占比下限（百分比）
};

void Usage(const char* argv0) {
    std::fprintf(stderr,
                 "用法：%s --model <path> [选项]\n"
                 "  --shot <png>          截图输出路径（默认 ppet_verify.png）\n"
                 "  --report <json>       JSON 报告路径（默认 ppet_verify.json）\n"
                 "  --width/--height <n>  视口尺寸（默认 480x640）\n"
                 "  --frames <n>          渲染帧数（默认 30）\n"
                 "  --min-coverage <pct>  非透明像素占比下限（默认 0.2%%）\n"
                 "支持：.vrm / .pmx / .zip / .model3.json / .moc3 / 目录\n",
                 argv0);
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (a == "--model") opt.model = next();
        else if (a == "--shot") opt.shot = next();
        else if (a == "--report") opt.report = next();
        else if (a == "--width") opt.width = std::atoi(next().c_str());
        else if (a == "--height") opt.height = std::atoi(next().c_str());
        else if (a == "--frames") opt.frames = std::atoi(next().c_str());
        else if (a == "--min-coverage") opt.minCoverage = std::atof(next().c_str());
        else if (a == "--help" || a == "-h") { Usage(argv[0]); return false; }
        else {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            Usage(argv[0]);
            return false;
        }
    }
    if (opt.model.empty()) {
        std::fprintf(stderr, "缺少 --model\n");
        Usage(argv[0]);
        return false;
    }
    opt.width = std::clamp(opt.width, 64, 4096);
    opt.height = std::clamp(opt.height, 64, 4096);
    opt.frames = std::clamp(opt.frames, 1, 600);
    return true;
}

std::string PlatformName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

std::string CompilerName() {
#if defined(__clang__)
    return "Clang " + std::string(__clang_version__);
#elif defined(__GNUC__)
    return "GCC " + std::string(__VERSION__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

const char* GlString(GLenum name) {
    const unsigned char* p = glGetString(name);
    return p ? reinterpret_cast<const char*>(p) : "";
}

/// 把 premultiplied-alpha 的渲染结果合成到浅灰棋盘背景上，便于人工查看。
void CompositeForPreview(std::vector<unsigned char>& rgba, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const unsigned char a = rgba[i + 3];
            const float af = a / 255.0f;
            const float inv = 1.0f - af;
            // 32px 棋盘： alternating two greys
            const unsigned char bg = ((x / 16 + y / 16) % 2 == 0) ? 0xB0 : 0x88;
            rgba[i + 0] = static_cast<unsigned char>(rgba[i + 0] + bg * inv);
            rgba[i + 1] = static_cast<unsigned char>(rgba[i + 1] + bg * inv);
            rgba[i + 2] = static_cast<unsigned char>(rgba[i + 2] + bg * inv);
            rgba[i + 3] = 255;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    ppet::api::Json report;
    report["tool"] = "ppet_render_verify";
    report["version"] = 1;
    report["platform"]["os"] = PlatformName();
    report["platform"]["compiler"] = CompilerName();
    report["requested_model"] = opt.model;

    ppet::Logger::Init("ppet_render_verify.log");

    auto finish = [&](int code) {
        report["result"] = code == 0 ? "PASS" : "FAIL";
        std::ofstream out(opt.report);
        if (out) out << report.dump(2, ' ', false, ppet::api::Json::error_handler_t::replace)
                     << "\n";
        ppet::Logger::Shutdown();
        return code;
    };

    // --- 1) GL 上下文 ---
    if (!glfwInit()) {
        report["errors"]["category"] = "环境错误";
        report["errors"]["details"] = {"GLFW 初始化失败（无图形环境？Linux 请用 xvfb-run）"};
        std::fprintf(stderr, "[render_verify] GLFW 初始化失败\n");
        return finish(1);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(opt.width, opt.height, "ppet_render_verify", nullptr, nullptr);
    if (!window) {
        report["errors"]["category"] = "环境错误";
        report["errors"]["details"] = {"创建 GLFW 窗口失败"};
        std::fprintf(stderr, "[render_verify] 创建窗口失败\n");
        glfwTerminate();
        return finish(1);
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        report["errors"]["category"] = "环境错误";
        report["errors"]["details"] = {"GLEW 初始化失败"};
        std::fprintf(stderr, "[render_verify] GLEW 初始化失败\n");
        glfwTerminate();
        return finish(1);
    }

    // 显卡信息：Windows N 卡环境下可以直接从报告里确认驱动/设备。
    report["platform"]["gl_vendor"] = GlString(GL_VENDOR);
    report["platform"]["gl_renderer"] = GlString(GL_RENDERER);
    report["platform"]["gl_version"] = GlString(GL_VERSION);
    std::printf("[render_verify] GPU: %s | %s | %s\n", GlString(GL_VENDOR),
                GlString(GL_RENDERER), GlString(GL_VERSION));

    // --- 2) 模型路径：zip 先整包解压 ---
    std::string modelPath = opt.model;
    bool fromZip = false;
    std::string sourceArchive;
    ppet::ModelPackage package;
    {
        std::string clean = modelPath;
        const std::size_t q = clean.find('?');
        if (q != std::string::npos) clean = clean.substr(0, q);
        if (ppet::FileExtension(clean) == "zip") {
            const std::string cache =
                (std::filesystem::temp_directory_path() / "ppet_verify_cache").string();
            std::string packageError;
            if (!ppet::ExtractModelPackage(clean, cache, package, packageError)) {
                ppet::ModelLoadError e = ppet::ModelLoadError::FromRaw(packageError, clean);
                report["errors"]["category"] = e.CategoryName();
                report["errors"]["details"] = ppet::api::Json::array({packageError});
                report["errors"]["advice"] = e.EffectiveAdvice();
                std::fprintf(stderr, "[render_verify] 模型包导入失败：%s\n",
                             packageError.c_str());
                glfwTerminate();
                return finish(1);
            }
            modelPath = package.modelPath;
            fromZip = true;
            sourceArchive = clean;
        }
    }

    // --- 3) 加载模型 ---
    const ppet::ModelType type = ppet::DetectModelType(modelPath);
    report["model"]["type"] = ppet::ModelTypeName(type);
    report["model"]["resolved_path"] = modelPath;
    report["model"]["from_zip"] = fromZip;
    report["model"]["source_archive"] = sourceArchive;

    ppet::ModelPtr model = ppet::CreateModel(type);
    if (!model) {
        report["errors"]["category"] =
            ppet::ModelErrorCategoryName(ppet::ModelErrorCategory::BackendUnavailable);
        report["errors"]["details"] = ppet::api::Json::array(
            {"该模型类型的后端不可用（Live2D 需开启 PPET_WITH_CUBISM）"});
        std::fprintf(stderr, "[render_verify] 后端不可用：%s\n",
                     ppet::ModelTypeName(type));
        glfwTerminate();
        return finish(1);
    }

    std::string rawError;
    if (!model->Load(modelPath, rawError)) {
        ppet::ModelLoadError e = ppet::ModelLoadError::FromRaw(rawError, modelPath);
        report["errors"]["category"] = e.CategoryName();
        report["errors"]["summary"] = e.summary;
        report["errors"]["details"] = e.details;
        report["errors"]["advice"] = e.EffectiveAdvice();
        std::fprintf(stderr, "[render_verify] 加载失败 [%s] %s\n", e.CategoryName(),
                     e.summary.c_str());
        glfwTerminate();
        return finish(1);
    }

    const ppet::ModelDebugInfo info = model->DebugInfo();
    report["model"]["display_name"] = info.displayName;
    report["model"]["format_version"] = info.formatVersion;
    report["model"]["bones"] = info.boneCount;
    report["model"]["vertices"] = info.vertexCount;
    report["model"]["triangles"] = info.triangleCount;
    report["model"]["materials"] = info.materialCount;
    report["model"]["textures_referenced"] = info.textureReferenced;
    report["model"]["textures_loaded"] = info.textureLoaded;
    report["model"]["missing_resources"] = info.missingResources;
    report["model"]["bone_sample"] = ppet::api::Json::array();
    for (std::size_t i = 0; i < info.bones.size() && i < 5; ++i) {
        report["model"]["bone_sample"].push_back(
            {{"index", info.bones[i].index},
             {"name", info.bones[i].name},
             {"parent", info.bones[i].parentIndex},
             {"depth", info.bones[i].depth}});
    }

    // --- 4) 渲染 ---
    glViewport(0, 0, opt.width, opt.height);
    ppet::RenderContext ctx;
    ctx.viewportWidth = opt.width;
    ctx.viewportHeight = opt.height;
    ctx.deltaTime = 1.0f / 60.0f;
    ctx.renderScale = 1.0f;
    ctx.hasGlobalCursor = true;
    ctx.globalCursor = ppet::Vec2(0.2f, 0.1f);
    ctx.localCursor = ppet::Vec2(0.2f, 0.1f);
    model->OnResize(opt.width, opt.height);

    for (int f = 0; f < opt.frames; ++f) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        model->Update(ctx);
        model->Render(ctx);
    }
    glFinish();
    // 渲染后再取一次诊断快照：Load() 刚结束时 gpuReady 还是 false，
    // 用渲染后的状态才能给出「是否正常渲染」的准确结论。
    const ppet::ModelDebugInfo renderedInfo = model->DebugInfo();

    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(opt.width) * opt.height * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const unsigned int glError = ppet::ConsumeGlErrors();

    long long covered = 0;
    long long colorful = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i + 3] > 10) ++covered;
        if (pixels[i] > 8 || pixels[i + 1] > 8 || pixels[i + 2] > 8) ++colorful;
    }
    const double coverage = 100.0 * static_cast<double>(covered) /
                            static_cast<double>(opt.width * opt.height);

    report["render"]["width"] = opt.width;
    report["render"]["height"] = opt.height;
    report["render"]["frames"] = opt.frames;
    report["render"]["non_transparent_pixels"] = covered;
    report["render"]["coverage_percent"] = std::round(coverage * 1000.0) / 1000.0;
    report["render"]["non_black_pixels"] = colorful;
    report["render"]["gl_error"] = glError;
    report["render"]["screenshot"] = opt.shot;
    report["render"]["model_status"] = renderedInfo.renderStatus;
    report["render"]["gpu_ready"] = renderedInfo.gpuReady;
    report["render"]["rendering_ok"] = renderedInfo.renderingOk;

    // 截图（合成到棋盘背景，透明区域可见）
    CompositeForPreview(pixels, opt.width, opt.height);
    stbi_flip_vertically_on_write(1);
    const bool pngOk = stbi_write_png(opt.shot.c_str(), opt.width, opt.height, 4,
                                      pixels.data(), opt.width * 4) != 0;
    report["render"]["screenshot_written"] = pngOk;

    // --- 5) 判定 ---
    std::vector<std::string> warnings;
    if (!info.missingResources.empty()) {
        warnings.push_back("缺失 " + std::to_string(info.missingResources.size()) +
                           " 个资源文件（PMX 单独导入常见，建议整包 .zip）");
    }
    if (type != ppet::ModelType::Live2D && info.boneCount == 0) {
        warnings.push_back("未找到有效骨架（骨骼数为 0）");
    }
    if (glError != 0) warnings.push_back("GL 错误码 " + std::to_string(glError));

    bool pass = pngOk && glError == 0 && coverage >= opt.minCoverage && covered > 0;
    // 覆盖率极低但确实画出了东西时给 WARN 而不是直接判死。
    if (!pass && covered > 0 && glError == 0) {
        warnings.push_back("非透明覆盖率 " + std::to_string(coverage) +
                           "% 低于阈值 " + std::to_string(opt.minCoverage) + "%");
    }
    report["warnings"] = warnings;
    report["result"] = pass ? (warnings.empty() ? "PASS" : "PASS_WITH_WARNINGS") : "FAIL";
    report["conclusion"] = pass
        ? "模型已加载并渲染出可见像素，GL 无错误"
        : "模型未通过渲染验证，详见 errors / warnings";

    std::printf("[render_verify] 模型 %s | 骨骼 %zu | 贴图 %zu/%zu | 缺失 %zu\n",
                ppet::ModelTypeName(type), info.boneCount, info.textureLoaded,
                info.textureReferenced, info.missingResources.size());
    std::printf("[render_verify] 非透明像素 %lld / %d（%.3f%%）| GL 错误 %u | 截图 %s\n",
                covered, opt.width * opt.height, coverage, glError,
                pngOk ? opt.shot.c_str() : "(失败)");
    std::printf("[render_verify] 结论：%s\n",
                report["result"].get<std::string>().c_str());
    for (const std::string& w : warnings) {
        std::printf("[render_verify]   警告：%s\n", w.c_str());
    }

    glfwTerminate();
    // 覆盖 finish() 里的默认 PASS/FAIL：本工具用更细的三态结论。
    std::ofstream out(opt.report);
    if (out) out << report.dump(2, ' ', false, ppet::api::Json::error_handler_t::replace)
                 << "\n";
    ppet::Logger::Shutdown();
    return pass ? 0 : 1;
}
