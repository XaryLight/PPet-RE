// PPet - C++ 移植版
// 功能验证工具：同一个模型在「中性 / 表情 / 口型 / 动作」四种状态下各渲染一遍，
// 分别截图，并与中性帧做像素差异对比，用来客观确认功能真的改变了画面。
//
// 【为什么每个状态都重新加载模型】
//   桌宠有呼吸、物理、随机待机动作，画面本来就在变。如果只是「先截一张、再改状态
//   截一张」，两张图的差异里混着待机动画，没法说明是表情/口型/动作起了作用。
//   这里每个状态都 model->Load() 重新加载，随机种子、物理摆锤、动作队列全部回到
//   相同初值，再用**相同的 dt 序列**推进相同帧数 —— 于是待机部分逐像素一致，
//   剩下的差异只能来自我们要验证的那个功能。
//
// 【用法】
//   ppet_feature_verify --model <path> --out <dir> [--prefix name]
//                       [--width 480] [--height 640] [--frames 30]
//
// 输出：<out>/<prefix>_{neutral,expression,lipsync,action}.png 与 _report.json
// 退出码：0 = 全部状态可判定；1 = 加载/渲染失败；2 = 环境错误。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

namespace fs = std::filesystem;
using ppet::api::Json;

struct Options {
    std::string model;
    std::string out = "feature_verify";
    std::string prefix = "feature";
    int width = 480;
    int height = 640;
    int frames = 30;
    /// 按模型实际帧宽高比自动调整渲染窗口宽度，避免横向模型被左右截断。
    bool autoSize = false;
};

void Usage(const char* argv0) {
    std::fprintf(stderr,
                 "用法：%s --model <path> [--out <dir>] [--prefix <name>]\n"
                 "        [--width 480] [--height 640] [--frames 30] [--auto-size]\n",
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
        else if (a == "--out") opt.out = next();
        else if (a == "--prefix") opt.prefix = next();
        else if (a == "--width") opt.width = std::atoi(next().c_str());
        else if (a == "--height") opt.height = std::atoi(next().c_str());
        else if (a == "--frames") opt.frames = std::atoi(next().c_str());
        else if (a == "--auto-size") opt.autoSize = true;
        else if (a == "--help" || a == "-h") { Usage(argv[0]); return false; }
        else { std::fprintf(stderr, "未知参数：%s\n", a.c_str()); return false; }
    }
    if (opt.model.empty()) { std::fprintf(stderr, "缺少 --model\n"); return false; }
    opt.width = std::clamp(opt.width, 64, 2048);
    opt.height = std::clamp(opt.height, 64, 2048);
    opt.frames = std::clamp(opt.frames, 4, 240);
    return true;
}

const char* GlString(GLenum name) {
    const auto* s = glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "";
}

/// 一帧的像素数据（RGBA，行序为 GL 的左下原点）。
struct Frame {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

/// 与基准帧的差异统计。
struct Diff {
    long long changed = 0;        ///< 任一通道差值 > 8 的像素数（只算非透明区域）
    long long comparable = 0;     ///< 两帧非透明像素的并集
    double changedPercent = 0.0;
    double meanAbsDiff = 0.0;     ///< 全通道平均绝对差（0~255）
    int minX = 0, minY = 0, maxX = 0, maxY = 0;  ///< 变化像素外接框（供局部放大对比）
};

Diff Compare(const Frame& a, const Frame& b) {
    Diff d;
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) return d;
    const int w = a.width;
    const int h = a.height;
    const std::size_t count = a.pixels.size() / 4;
    long long sum = 0;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (std::size_t p = 0; p < count; ++p) {
        const std::size_t i = p * 4;
        const bool visible = a.pixels[i + 3] > 10 || b.pixels[i + 3] > 10;
        if (!visible) continue;   // 全透明背景不参与比较
        ++d.comparable;
        const int dr = std::abs(static_cast<int>(a.pixels[i]) - b.pixels[i]);
        const int dg = std::abs(static_cast<int>(a.pixels[i + 1]) - b.pixels[i + 1]);
        const int db = std::abs(static_cast<int>(a.pixels[i + 2]) - b.pixels[i + 2]);
        const int da = std::abs(static_cast<int>(a.pixels[i + 3]) - b.pixels[i + 3]);
        sum += dr + dg + db + da;
        if (dr > 8 || dg > 8 || db > 8 || da > 8) {
            ++d.changed;
            const int x = static_cast<int>(p % static_cast<std::size_t>(w));
            const int y = static_cast<int>(p / static_cast<std::size_t>(w));
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    d.changedPercent = d.comparable > 0
                           ? 100.0 * static_cast<double>(d.changed) / d.comparable
                           : 0.0;
    d.meanAbsDiff = count > 0 ? static_cast<double>(sum) / (count * 4) : 0.0;
    if (maxX >= minX) {
        d.minX = minX; d.minY = minY; d.maxX = maxX; d.maxY = maxY;
    }
    return d;
}

bool WritePng(const std::string& path, const Frame& frame) {
    // GL 读回是左下原点；PNG 需要左上原点，逐行翻转。
    std::vector<unsigned char> flipped(frame.pixels.size());
    const std::size_t stride = static_cast<std::size_t>(frame.width) * 4;
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * stride,
                    frame.pixels.data() +
                        static_cast<std::size_t>(frame.height - 1 - y) * stride,
                    stride);
    }
    stbi_flip_vertically_on_write(0);
    return stbi_write_png(path.c_str(), frame.width, frame.height, 4, flipped.data(),
                          static_cast<int>(stride)) != 0;
}

/// 应用某个状态的功能。frame == 0 时做一次性触发，其余帧做持续性设置。
void ApplyState(ppet::IModel* model, const std::string& state, int frame) {
    if (state == "expression") {
        if (frame == 0) {
            // 统一用「手动切到 happy 表情」这一个入口验证表情功能：
            //   - PMX / VRM：走表情 morph 权重；
            //   - Live2D：优先播表情动作组，没有动作组时回退到参数级表情预设
            //     （ParamBrow* / ParamEye*Smile / ParamCheek 等）。
            // 不额外调 TriggerRandomExpression()，否则随机表情会覆盖掉 happy，
            // 截图与「表情 -> happy」不再有确定的对应关系。
            model->SetExpressionPreset("happy");
        }
    } else if (state == "lipsync") {
        // 口型需要每帧写入：App 的 TickDialogue 也是每帧把开口度刷进模型。
        // Live2D 走 SetLipSync(ParamMouthOpenY)，PMX/VRM 走元音 morph 权重。
        const float open = 0.9f + 0.08f * std::sin(static_cast<float>(frame) * 0.7f);
        model->SetLipSync(open, 0.35f);
        model->SetExpressionWeights({{"aa", 0.9f}, {"ih", 0.25f}, {"ou", 0.1f}});
    } else if (state == "action") {
        if (frame == 0) {
            const std::vector<std::string> names = model->MotionNames();
            // 别挑 mail / 特效类动作：这类动作会大幅改变画布缩放或部件不透明度，
            // 截图变成一块放大的色块，无法代表「肢体动作是否生效」。
            // 优先点击互动（touch/tap），其次待机，最后才用第一个。
            std::string chosen;
            for (const std::string& n : names) {
                std::string lower = n;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("touch") != std::string::npos ||
                    lower.find("tap") != std::string::npos) {
                    chosen = n;
                    break;
                }
            }
            if (chosen.empty()) {
                for (const std::string& n : names) {
                    std::string lower = n;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (lower.find("idle") != std::string::npos) { chosen = n; break; }
                }
            }
            if (chosen.empty() && !names.empty()) chosen = names.front();
            if (!chosen.empty()) model->PlayMotion(chosen);
        }
    }
}

/// 重新加载模型并渲染一个状态，返回最后一帧。
bool RenderState(ppet::IModel* model, const std::string& modelPath, const Options& opt,
                 const std::string& state, Frame& outFrame,
                 std::vector<std::pair<std::string, float>>& outSnapshot,
                 std::string& error) {
    std::string rawError;
    if (!model->Load(modelPath, rawError)) {
        error = rawError;
        return false;
    }
    model->OnResize(opt.width, opt.height);

    ppet::RenderContext ctx;
    ctx.viewportWidth = opt.width;
    ctx.viewportHeight = opt.height;
    ctx.deltaTime = 1.0f / 60.0f;
    ctx.renderScale = 1.0f;
    // 固定的光标位置：避免视线追踪引入不确定性。
    ctx.hasGlobalCursor = true;
    ctx.globalCursor = ppet::Vec2(0.15f, -0.1f);
    ctx.localCursor = ppet::Vec2(0.15f, -0.1f);
    ctx.hasWindowCursor = true;
    ctx.windowCursor = ppet::Vec2(0.15f, -0.1f);

    outFrame.width = opt.width;
    outFrame.height = opt.height;
    outFrame.pixels.assign(static_cast<std::size_t>(opt.width) * opt.height * 4, 0);

    for (int f = 0; f < opt.frames; ++f) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ApplyState(model, state, f);   // 与 App 里 TickDialogue -> Update 的顺序一致
        model->Update(ctx);
        model->Render(ctx);
    }
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE,
                 outFrame.pixels.data());
    while (glGetError() != GL_NO_ERROR) {
    }
    outSnapshot = model->ParameterSnapshot();
    return true;
}

/// 参数级证据：同名参数的变化数量与累计变化量。
struct ParamDiff {
    int changed = 0;
    double totalDelta = 0.0;
    std::vector<std::string> samples;  ///< 前几个变化最大的参数，形如 "name: a -> b"
};

ParamDiff DiffSnapshot(const std::vector<std::pair<std::string, float>>& a,
                       const std::vector<std::pair<std::string, float>>& b) {
    ParamDiff out;
    if (a.empty() || b.empty()) return out;
    std::map<std::string, float> before;
    for (const auto& kv : a) before[kv.first] = kv.second;
    std::vector<std::pair<double, std::string>> deltas;
    for (const auto& kv : b) {
        auto it = before.find(kv.first);
        if (it == before.end()) continue;
        const double delta = std::abs(static_cast<double>(kv.second) - it->second);
        if (delta <= 1e-4) continue;
        ++out.changed;
        out.totalDelta += delta;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: %.3f -> %.3f", kv.first.c_str(),
                      it->second, kv.second);
        deltas.emplace_back(delta, buf);
    }
    std::sort(deltas.begin(), deltas.end(),
              [](const auto& x, const auto& y) { return x.first > y.first; });
    for (std::size_t i = 0; i < deltas.size() && i < 8; ++i) {
        out.samples.push_back(deltas[i].second);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    ppet::Logger::Init("ppet_feature_verify.log");
    Json report;
    report["tool"] = "ppet_feature_verify";
    report["model_path"] = opt.model;
    report["frames_per_state"] = opt.frames;
    report["states"] = Json::array();

    auto finish = [&](int code) {
        report["result"] = code == 0 ? "PASS" : "FAIL";
        std::error_code ec;
        fs::create_directories(opt.out, ec);
        const std::string reportPath =
            (fs::path(opt.out) / (opt.prefix + "_report.json")).string();
        std::ofstream out(reportPath);
        if (out) out << report.dump(2, ' ', false, Json::error_handler_t::replace) << "\n";
        ppet::Logger::Shutdown();
        return code;
    };

    // --- 模型路径（zip 整包解压）与后端创建 ---
    // 必须在 GL 窗口创建之前完成：--auto-size 要先读到模型实际帧宽高比，
    // 才能决定窗口应该多宽，否则横向模型在固定竖窗口里会被左右截断。
    std::string modelPath = opt.model;
    {
        std::string clean = modelPath;
        const std::size_t q = clean.find('?');
        if (q != std::string::npos) clean = clean.substr(0, q);
        if (ppet::FileExtension(clean) == "zip") {
            ppet::ModelPackage package;
            std::string packageError;
            const std::string cache =
                (fs::temp_directory_path() / "ppet_feature_verify_cache").string();
            if (!ppet::ExtractModelPackage(clean, cache, package, packageError)) {
                report["error"] = "模型包导入失败：" + packageError;
                return finish(1);
            }
            modelPath = package.modelPath;
        }
    }

    const ppet::ModelType type = ppet::DetectModelType(modelPath);
    report["model_type"] = ppet::ModelTypeName(type);
    ppet::ModelPtr model = ppet::CreateModel(type);
    if (!model) {
        report["error"] = "模型后端不可用（Live2D 需 Cubism SDK）";
        return finish(1);
    }

    if (opt.autoSize) {
        std::string probeError;
        if (model->Load(modelPath, probeError)) {
            const float aspect = model->PreferredFrameAspect();
            if (aspect > 0.05f && aspect < 20.0f) {
                const int before = opt.width;
                opt.width = std::clamp(
                    static_cast<int>(static_cast<float>(opt.height) * aspect), 64, 2048);
                report["auto_size"] = true;
                report["auto_size_aspect"] = aspect;
                report["auto_size_width"] = opt.width;
                report["auto_size_width_before"] = before;
            }
        }
    }
    report["width"] = opt.width;
    report["height"] = opt.height;

    // --- GL 上下文 ---
    if (!glfwInit()) {
        report["error"] = "GLFW 初始化失败（Linux 请用 xvfb-run）";
        return finish(2);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(opt.width, opt.height, "ppet_feature_verify", nullptr, nullptr);
    if (!window) {
        report["error"] = "创建 GLFW 窗口失败";
        glfwTerminate();
        return finish(2);
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        report["error"] = "GLEW 初始化失败";
        glfwTerminate();
        return finish(2);
    }
    report["gpu"] = GlString(GL_RENDERER);

    std::error_code ec;
    fs::create_directories(opt.out, ec);

    const std::vector<std::string> states = {"neutral", "expression", "lipsync", "action"};
    std::vector<Frame> frames(states.size());
    std::vector<std::vector<std::pair<std::string, float>>> snapshots(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        std::string error;
        if (!RenderState(model.get(), modelPath, opt, states[i], frames[i], snapshots[i],
                         error)) {
            report["error"] = "状态 " + states[i] + " 渲染失败：" + error;
            glfwTerminate();
            return finish(1);
        }
        const std::string png =
            (fs::path(opt.out) / (opt.prefix + "_" + states[i] + ".png")).string();
        if (!WritePng(png, frames[i])) {
            report["error"] = "写 PNG 失败：" + png;
            glfwTerminate();
            return finish(1);
        }
        Json entry;
        entry["state"] = states[i];
        entry["png"] = png;
        report["states"].push_back(entry);
        std::printf("[feature_verify] %-12s -> %s\n", states[i].c_str(), png.c_str());
    }

    // --- 与中性帧对比 ---
    Json comparisons = Json::object();
    for (std::size_t i = 1; i < states.size(); ++i) {
        const Diff d = Compare(frames[0], frames[i]);
        const ParamDiff pd = DiffSnapshot(snapshots[0], snapshots[i]);
        Json c;
        c["baseline"] = "neutral";
        c["changed_pixels"] = d.changed;
        c["comparable_pixels"] = d.comparable;
        c["changed_percent"] = std::round(d.changedPercent * 100.0) / 100.0;
        c["mean_abs_diff"] = std::round(d.meanAbsDiff * 1000.0) / 1000.0;
        c["change_bbox"] = Json::array({d.minX, d.minY, d.maxX, d.maxY});
        c["param_changed_count"] = pd.changed;
        c["param_total_delta"] = std::round(pd.totalDelta * 1000.0) / 1000.0;
        c["param_samples"] = pd.samples;
        // 像素差异或参数变化任一成立即判定功能生效。
        // Live2D 的嘴部/眉毛只占画面极小区域，像素占比常低于 0.5%，
        // 但参数快照能客观证明口型/表情确实被写进了模型。
        c["pass"] = d.changedPercent >= 0.5 || pd.totalDelta > 0.01;
        comparisons[states[i]] = c;
        std::printf("[feature_verify] %-12s 像素 %.2f%% (%lld px)，参数 %d 项变化/累计 %.3f -> %s\n",
                    states[i].c_str(), d.changedPercent, d.changed, pd.changed,
                    pd.totalDelta, (d.changedPercent >= 0.5 || pd.totalDelta > 0.01)
                                       ? "PASS" : "FAIL");
    }
    report["comparisons"] = comparisons;

    // 能力清单（控制台 action / expression 会用到）。
    report["motion_names"] = model->MotionNames();
    report["expression_presets"] = model->ExpressionPresetIds();
    report["debug_info"] = Json{{"display_name", model->DebugInfo().displayName},
                                {"motion_status", model->DebugInfo().motionStatus},
                                {"expression_status", model->DebugInfo().expressionStatus},
                                {"render_status", model->DebugInfo().renderStatus}};

    glfwTerminate();
    return finish(0);
}
