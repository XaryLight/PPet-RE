// PPet - C++ 移植版
// 设置界面实现：独立窗口 + Dear ImGui。
//
// 设计要点（详见 SettingsPanel.hpp 顶部注释）：
//   - 创建一个 800x600 的普通窗口（不透明、有标题栏），拥有独立 GL 上下文；
//   - ImGui 上下文全局唯一，渲染只发生在设置窗口的 GL 上下文里；
//   - Render() 内部完成「切到设置上下文 -> 绘制 -> 切回主上下文」，调用方无需关心切换。
#include "ui/SettingsPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

// GLEW 必须排在 GLFW 之前：glfw3.h 会先包含 GL/gl.h，
// 而 glew.h 要求自己是第一个包含 GL 头的文件，否则报
// “gl.h included before glew.h”。
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "platform/FileDialog.hpp"
#include "platform/Window.hpp"

namespace ppet {

namespace {

/// 探测是否存在某个 CJK 字体文件（默认位图字体不含中文，必须合并一个 CJK 字体）。
bool FileExists(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec);
}

/// 按平台列出常见的 CJK 字体候选路径，找到一个就用它合并进 ImGui 字体。
const char* FindCjkFont() {
    static const char* kCandidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\msjh.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
#endif
    };
    for (const char* p : kCandidates) {
        if (FileExists(p)) return p;
    }
    return nullptr;
}

}  // namespace

struct SettingsPanel::Impl {
    GLFWwindow* window = nullptr;
    GLFWwindow* mainWindow = nullptr;
    bool initialized = false;
    bool visible = false;
};

SettingsPanel::SettingsPanel() : impl_(std::make_unique<Impl>()) {}

SettingsPanel::~SettingsPanel() { Shutdown(); }

bool SettingsPanel::Initialize(Window* mainWindow, std::string& error) {
    if (impl_->initialized) return true;

    // GLFW 已由主窗口初始化；不要再次 glfwInit/glfwTerminate，否则会破坏主窗口。
    impl_->mainWindow = mainWindow ? mainWindow->Handle() : nullptr;

    // 与主窗口一致的 3.3 Core 上下文，保证 ImGui 的 GL3 后端可用。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);  // 设置窗口默认置顶

    GLFWwindow* w = glfwCreateWindow(800, 600, "PPet", nullptr, nullptr);
    if (!w) {
        error = "无法创建设置窗口";
        return false;
    }
    impl_->window = w;

    // 新窗口拥有独立 GL 上下文，必须在此上下文里初始化 GLEW 与 ImGui 后端。
    glfwMakeContextCurrent(w);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        error = "设置窗口 GLEW 初始化失败";
        return false;
    }

    // ImGui 全局唯一上下文。
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 不写 imgui.ini：桌面程序不该在工作目录里留下配置文件。
    io.IniFilename = nullptr;

    // 默认字体不含中文，合并一个 CJK 字体使界面中文可见。
    io.Fonts->AddFontDefault();
    if (const char* cjk = FindCjkFont()) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(cjk, 16.0f, &cfg,
                                     io.Fonts->GetGlyphRangesChineseFull());
    }

    // GLFW 后端绑定到设置窗口。
    // 注意顺序：SetCallbacksChainForAllWindows 依赖后端数据，
    // 而后端数据要到 InitForOpenGL 之后才存在，先调用会解引用空指针。
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    // 开启「全窗口回调链」，让 ImGui 转发事件时保留主窗口既有的拖动/点击回调。
    ImGui_ImplGlfw_SetCallbacksChainForAllWindows(true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    glfwHideWindow(w);

    // 还原主窗口上下文，保证主循环首帧在主上下文里绘制。
    if (impl_->mainWindow) glfwMakeContextCurrent(impl_->mainWindow);

    impl_->initialized = true;
    return true;
}

void SettingsPanel::Shutdown() {
    if (!impl_->initialized) return;
    if (impl_->window) {
        glfwMakeContextCurrent(impl_->window);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(impl_->window);
        if (impl_->mainWindow) glfwMakeContextCurrent(impl_->mainWindow);
        impl_->window = nullptr;
    }
    impl_->initialized = false;
}

void SettingsPanel::Render() {
    if (!impl_->initialized || !impl_->visible) return;

    // 用户点了关闭按钮：隐藏而非销毁，保留界面状态。
    if (glfwWindowShouldClose(impl_->window)) {
        glfwSetWindowShouldClose(impl_->window, GLFW_FALSE);
        Hide();
        return;
    }

    glfwMakeContextCurrent(impl_->window);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    BuildUi();

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(impl_->window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.16f, 0.16f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(impl_->window);

    // 还原主窗口上下文。
    if (impl_->mainWindow) glfwMakeContextCurrent(impl_->mainWindow);
}

void SettingsPanel::BuildUi() {
    if (!config || !strings) return;
    const Strings& s = *strings;
    AppConfig& cfg = *config;

    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin(s.settingsTitle.c_str(), nullptr, ImGuiWindowFlags_None);

    // ---------------- 模型 ----------------
    if (ImGui::CollapsingHeader(s.modelList.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& list = cfg.modelList;
        if (list.empty()) {
            ImGui::TextUnformatted(s.noModel.c_str());
        } else {
            if (ImGui::BeginChild("model_list", ImVec2(0, 120), true)) {
                for (size_t i = 0; i < list.size(); ++i) {
                    const bool selected = (list[i] == cfg.modelPath);
                    if (ImGui::Selectable(list[i].c_str(), selected)) {
                        if (onSelectModel) onSelectModel(static_cast<int>(i));
                    }
                }
            }
            ImGui::EndChild();
        }

        if (ImGui::Button(s.importModel.c_str())) {
            std::string path, dlgError;
            if (OpenFileDialog(s.importModel, s.importModel, s.modelFileFilter, "", nullptr,
                              path, dlgError)) {
                if (onImportModel) onImportModel(path);
            } else if (!dlgError.empty()) {
                std::fprintf(stderr, "[PPet] %s\n", dlgError.c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(s.removeModel.c_str())) {
            const auto it = std::find(list.begin(), list.end(), cfg.modelPath);
            if (it != list.end() && onRemoveModel) {
                onRemoveModel(static_cast<int>(std::distance(list.begin(), it)));
            }
        }

        static char urlBuf[512] = {0};
        ImGui::TextUnformatted(s.onlineModelUrl.c_str());
        ImGui::InputText("##online_url", urlBuf, sizeof(urlBuf));
        ImGui::SameLine();
        if (ImGui::Button(s.importOnlineModel.c_str())) {
            const std::string url(urlBuf);
            if (!url.empty() && onImportModel) onImportModel(url);
        }
    }

    // ---------------- 渲染大小 ----------------
        ImGui::TextUnformatted(s.renderSize.c_str());
        ImGui::SetNextItemWidth(-1);
        float scale = cfg.renderScale;
        if (ImGui::SliderFloat("##render_scale", &scale, 0.25f, 4.0f, "%.2f×")) {
            cfg.renderScale = scale;
            if (onConfigChanged) onConfigChanged();
        }

        // ---------------- 全屏追踪 ----------------
    if (ImGui::CollapsingHeader(s.fullscreenTracking.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        bool ft = cfg.fullscreenTracking;
        if (ImGui::Checkbox(s.fullscreenTracking.c_str(), &ft)) {
            cfg.fullscreenTracking = ft;
            if (onConfigChanged) onConfigChanged();
        }
        float radius = cfg.trackingRadius;
        ImGui::TextUnformatted(s.trackingRadius.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##tracking_radius", &radius, 50.0f, 2000.0f, "%.0f px")) {
            cfg.trackingRadius = radius;
            if (onConfigChanged) onConfigChanged();
        }
        float angle = cfg.maxTrackingAngle;
        ImGui::TextUnformatted(s.maxTrackingAngle.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##tracking_angle", &angle, 0.0f, 90.0f, "%.0f°")) {
            cfg.maxTrackingAngle = angle;
            if (onConfigChanged) onConfigChanged();
        }
        ImGui::TextWrapped("%s", s.trackingHint.c_str());
    }

    // ---------------- 窗口 ----------------
    if (ImGui::CollapsingHeader(s.windowSection.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        bool b = cfg.alwaysOnTop;
        if (ImGui::Checkbox(s.alwaysOnTopLabel.c_str(), &b)) {
            cfg.alwaysOnTop = b;
            if (onConfigChanged) onConfigChanged();
        }
        b = cfg.ignoreMouseEvents;
        if (ImGui::Checkbox(s.ignoreMouseLabel.c_str(), &b)) {
            cfg.ignoreMouseEvents = b;
            if (onConfigChanged) onConfigChanged();
        }
        b = cfg.showTool;
        if (ImGui::Checkbox(s.showToolLabel.c_str(), &b)) {
            cfg.showTool = b;
            if (onConfigChanged) onConfigChanged();
        }
    }

    // ---------------- 语言 ----------------
    if (ImGui::CollapsingHeader(s.language.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* langs[] = {"中文", "English"};
        int lang = (cfg.language == Language::Chinese) ? 0 : 1;
        ImGui::TextUnformatted(s.language.c_str());
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("##language", &lang, langs, 2)) {
            cfg.language = (lang == 0) ? Language::Chinese : Language::English;
            if (onConfigChanged) onConfigChanged();
        }
    }

    // ---------------- 关于 ----------------
    if (ImGui::CollapsingHeader(s.about.c_str())) {
        ImGui::TextWrapped("%s", s.aboutText.c_str());
    }

    // ---------------- 危险操作 ----------------
    ImGui::Separator();
    if (ImGui::Button(s.clearSettings.c_str())) {
        if (onClearSettings) onClearSettings();
    }

    ImGui::End();
}

bool SettingsPanel::IsVisible() const { return impl_->visible; }

void SettingsPanel::Show() {
    if (!impl_->initialized) return;
    impl_->visible = true;
    glfwShowWindow(impl_->window);
}

void SettingsPanel::Hide() {
    impl_->visible = false;
    if (impl_->window) glfwHideWindow(impl_->window);
}

void SettingsPanel::Toggle() {
    if (impl_->visible)
        Hide();
    else
        Show();
}

GLFWwindow* SettingsPanel::Handle() const { return impl_->window; }

}  // namespace ppet
