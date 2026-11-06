// PPet - C++ 移植版
// 应用主控制器实现。
#include "app/App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <GLFW/glfw3.h>

#include "core/Math.hpp"
#include "core/FileIO.hpp"
#include "core/Log.hpp"
#include "core/ModelPackage.hpp"
#include "platform/AutoStart.hpp"
#include "platform/OpenExternal.hpp"
#include "render/RenderUtils.hpp"

namespace ppet {
namespace {

/// 菜单项 id 约定（与原 Electron 版 tray.ts 保持一致）。
constexpr const char* kIdAlwaysOnTop = "alwaysOnTop";
constexpr const char* kIdShowTool = "showTool";
constexpr const char* kIdIgnoreMouse = "ignoreMouseEvents";
constexpr const char* kIdSettings = "settings";
constexpr const char* kIdPrevModel = "prevModel";
constexpr const char* kIdNextModel = "nextModel";
constexpr const char* kIdImportModel = "importModel";
constexpr const char* kIdLanguage = "language";
constexpr const char* kIdLangZh = "lang.zh";
constexpr const char* kIdLangEn = "lang.en";
constexpr const char* kIdReload = "reload";
constexpr const char* kIdDebug = "debug";
constexpr const char* kIdOpenAtLogin = "openAtLogin";
constexpr const char* kIdFeedback = "feedback";
constexpr const char* kIdAbout = "about";
constexpr const char* kIdQuit = "quit";

/// 从 URL 字符串中提取模型路径（去掉查询参数）。
std::string StripQuery(const std::string& url) {
    const std::size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q);
}

}  // namespace

App::~App() { Shutdown(); }

bool App::Initialize(std::string& error) {
    // --- 配置 ---
    configPath_ = ConfigStore::DefaultConfigPath();
    // 首次启动（配置文件不存在）时自动弹出设置面板，
    // 否则新用户面对的是一个透明的空窗口，不知道该做什么。
    const bool firstLaunch = !std::filesystem::exists(configPath_);
    if (!configStore_.Load(configPath_, error)) {
        // 配置损坏不应阻止启动：记录错误并使用默认配置继续。
        PPET_LOG_ERR("%s\n", error.c_str());
        error.clear();
    }
    strings_ = GetStrings(configStore_.config().language);
    currentLanguage_ = configStore_.config().language;
    showTool_ = configStore_.config().showTool;

    // --- 窗口 ---
    WindowOptions opts;
    opts.width = configStore_.config().windowWidth;
    opts.height = configStore_.config().windowHeight;
    opts.x = configStore_.config().windowX;
    opts.y = configStore_.config().windowY;
    opts.title = "PPet";
    opts.alwaysOnTop = configStore_.config().alwaysOnTop;
    opts.transparent = true;
    opts.resizable = false;
    opts.skipTaskbar = true;

    if (!window_.Create(opts, error)) return false;

    // --- 输入回调 ---
    window_.onMouseButton = [this](int button, int action) {
        // 忽略点击开启时不响应交互，但拖动仍需处理（否则用户无法调整位置）。
        if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT) {
            if (configStore_.config().ignoreMouseEvents) return;
            const Vec2 pos = window_.Position();
            const Vec2 size = window_.Size();
            dragging_ = true;
            dragOffsetX_ = 0.0;
            dragOffsetY_ = 0.0;
            (void)pos;
            (void)size;
        } else if (action == GLFW_RELEASE && button == GLFW_MOUSE_BUTTON_LEFT) {
            dragging_ = false;
            if (windowMovedByUser_) {
                windowMovedByUser_ = false;
                SaveConfig();
            }
        }
    };

    window_.onMouseMove = [this](double xpos, double ypos) {
        if (!dragging_) return;
        // 用光标在窗口内的位置减去按下时的偏移，实现无跳动拖动。
        const double dx = xpos - dragOffsetX_;
        const double dy = ypos - dragOffsetY_;
        const Vec2 cur = window_.Position();
        window_.SetPosition(static_cast<int>(cur.x + dx), static_cast<int>(cur.y + dy));
        dragOffsetX_ = xpos;
        dragOffsetY_ = ypos;
        windowMovedByUser_ = true;
    };

    window_.onResize = [this](int, int) {
        // 尺寸变化时通知模型重新取景。
        const Vec2 size = window_.Size();
        if (model_) model_->OnResize(static_cast<int>(size.x), static_cast<int>(size.y));
    };

    window_.onClose = [this]() { running_ = false; };

    // --- 全局鼠标追踪（全屏追踪的核心）---
    if (configStore_.config().fullscreenTracking) {
        std::string cursorError;
        if (!cursorTracker_.Start(cursorError)) {
            // 追踪失败不应阻止启动，降级为窗口内追踪。
            PPET_LOG_ERR("全局鼠标追踪不可用：%s\n", cursorError.c_str());
            configStore_.config().fullscreenTracking = false;
        }
    }

    // --- 托盘 ---
    std::string trayError;
    if (!tray_.Create(trayError)) {
        // Linux 下没有托盘宿主是正常情况，程序继续运行。
        PPET_LOG_ERR("%s\n", trayError.c_str());
    }
    tray_.SetItemCallback([this](const std::string& id, bool checked) {
        OnTrayItem(id, checked);
    });

    ApplyConfig();
    RebuildTrayMenu();

    // --- 设置界面（独立 Qt 6 窗口）---
    std::string settingsError;
    settingsPanel_.config = &configStore_.config();
    settingsPanel_.strings = &strings_;
    settingsPanel_.onConfigChanged = [this]() {
        SaveConfig();
        ApplyConfig();
        RefreshLanguageIfNeeded();
    };
    settingsPanel_.onImportModel = [this](const std::string& path) {
        std::string err;
        if (!LoadModel(path, err)) {
            PPET_LOG_ERR("%s: %s\n", strings_.importFailed.c_str(), err.c_str());
        } else {
            SaveConfig();
        }
    };
    settingsPanel_.onRemoveModel = [this](int index) {
        auto& list = configStore_.config().modelList;
        if (index >= 0 && index < static_cast<int>(list.size())) {
            list.erase(list.begin() + index);
            SaveConfig();
        }
    };
    settingsPanel_.onSelectModel = [this](int index) {
        const auto& list = configStore_.config().modelList;
        if (index >= 0 && index < static_cast<int>(list.size())) {
            std::string err;
            if (!LoadModel(list[index], err)) {
                PPET_LOG_ERR("%s: %s\n", strings_.modelLoadFailed.c_str(),
                             err.c_str());
            }
        }
    };
    settingsPanel_.onClearSettings = [this]() {
        // 保留当前模型，其余配置恢复默认。
        const std::string keepModel = configStore_.config().modelPath;
        configStore_.config() = AppConfig();
        configStore_.config().modelPath = keepModel;
        currentLanguage_ = configStore_.config().language;
        strings_ = GetStrings(currentLanguage_);
        settingsPanel_.Refresh();
        SaveConfig();
        ApplyConfig();
        RebuildTrayMenu();
    };
    if (!settingsPanel_.Initialize(settingsError)) {
        // 设置界面不可用不应阻止启动：记录错误并继续。
        PPET_LOG_ERR("%s: %s\n", strings_.settings.c_str(), settingsError.c_str());
    } else if (firstLaunch) {
        settingsPanel_.Show();
    }

    // --- 模型 ---
    const auto& cfg = configStore_.config();
    if (!cfg.modelPath.empty()) {
        std::string modelError;
        if (!LoadModel(cfg.modelPath, modelError)) {
            PPET_LOG_ERR("%s: %s\n", strings_.modelLoadFailed.c_str(),
                         modelError.c_str());
        }
    }

    // 没有任何模型可渲染时，透明主窗口在桌面上完全不可见。
    // 为避免「程序启动了却什么都没有」的困惑，这里保证设置窗口可见，
    // 让用户有入口去导入模型。设置面板初始化失败时 Show() 会自动 no-op，安全。
    if (configStore_.config().modelPath.empty()) {
        settingsPanel_.Show();
    }

    running_ = true;
    return true;
}

void App::ApplyConfig() {
    const auto& cfg = configStore_.config();
    window_.SetAlwaysOnTop(cfg.alwaysOnTop);
    window_.SetIgnoreMouseEvents(cfg.ignoreMouseEvents);

    if (cfg.fullscreenTracking && !cursorTracker_.IsRunning()) {
        std::string err;
        cursorTracker_.Start(err);
    } else if (!cfg.fullscreenTracking && cursorTracker_.IsRunning()) {
        cursorTracker_.Stop();
    }
}

void App::RefreshLanguageIfNeeded() {
    if (configStore_.config().language == currentLanguage_) return;
    currentLanguage_ = configStore_.config().language;
    strings_ = GetStrings(currentLanguage_);
    // 设置面板直接用最新 strings_ 重绘文案。
    settingsPanel_.Refresh();
    RebuildTrayMenu();
}

std::string App::ModelCacheDirectory() const {
    // 与配置文件同级的 models 目录：Windows 下是 %APPDATA%/PPet/models。
    const std::string configDir = ParentDirectory(configPath_);
    if (configDir.empty()) return "models";
    return JoinPath(configDir, "models");
}

bool App::LoadModel(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "模型路径为空";
        return false;
    }

    // 在线模型地址去掉查询参数后再判断类型（在线地址常带 ?token=xxx）。
    std::string cleanPath = StripQuery(path);

    // 压缩包导入：先把 zip 解压到缓存目录，再定位其中的模型文件。
    // 社区里的 Live2D / MMD 模型通常以 zip 形式分发，逐个文件导入体验很差。
    if (FileExtension(cleanPath) == "zip") {
        ModelPackage package;
        std::string packageError;
        if (!ExtractModelPackage(cleanPath, ModelCacheDirectory(), package, packageError)) {
            error = packageError;
            return false;
        }
        cleanPath = package.modelPath;
    }

    const ModelType type = DetectModelType(cleanPath);
    if (type == ModelType::Unknown) {
        error = "无法识别的模型格式（支持 .json / .pmx / .vrm / .zip）";
        return false;
    }

    ModelPtr model = CreateModel(type);
    if (!model) {
        if (type == ModelType::Live2D) {
            error = "Live2D 模型需要编译 Cubism SDK 后端（CMake 选项 PPET_WITH_CUBISM）";
        } else {
            error = "该模型类型的后端不可用";
        }
        return false;
    }

    if (!model->Load(cleanPath, error)) {
        return false;
    }

    model_ = model;
    const Vec2 size = window_.Size();
    model_->OnResize(static_cast<int>(size.x), static_cast<int>(size.y));

    auto& cfg = configStore_.config();
    // 保存「实际加载的模型文件」路径：zip 导入时这是解压后的模型文件，
    // 这样下次启动能直接加载，而不必再走一次解压（缓存目录也会被保留）。
    cfg.modelPath = cleanPath;
    // 新模型加入列表并置为当前。
    if (std::find(cfg.modelList.begin(), cfg.modelList.end(), cleanPath) ==
        cfg.modelList.end()) {
        cfg.modelList.push_back(cleanPath);
    }
    return true;
}

void App::RebuildTrayMenu() {
    const auto& cfg = configStore_.config();
    strings_ = GetStrings(cfg.language);

    using Item = TrayMenuItem;

    std::vector<Item> items;
    // 字段顺序：type, label, id, checked, enabled, subItems, accelerator
    items.push_back({Item::Type::Checkbox, strings_.alwaysOnTop, kIdAlwaysOnTop,
                     cfg.alwaysOnTop, true, {}, "Ctrl+T"});
    items.push_back({Item::Type::Checkbox, strings_.tools, kIdShowTool, showTool_, true, {},
                     "Ctrl+B"});
    items.push_back({Item::Type::Checkbox, strings_.ignoreMouseEvents, kIdIgnoreMouse,
                     cfg.ignoreMouseEvents, true, {}, "Ctrl+I"});
    items.push_back({Item::Type::Action, strings_.settings, kIdSettings, false, true, {},
                     "Ctrl+,"});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.prevModel, kIdPrevModel, false, true, {},
                     "Ctrl+P"});
    items.push_back({Item::Type::Action, strings_.nextModel, kIdNextModel, false, true, {},
                     "Ctrl+N"});
    // 导入模型：当前没有独立设置窗口，这里直接用系统文件对话框加载，
    // 让桌宠在透明窗口里至少能显示出来（否则窗口内容为空）。
    items.push_back({Item::Type::Action, strings_.importModel, kIdImportModel, false, true, {},
                     "Ctrl+O"});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});

    // 语言子菜单
    {
        std::vector<Item> langItems;
        langItems.push_back({Item::Type::Radio, "简体中文", kIdLangZh,
                             cfg.language == Language::Chinese, true, {}, ""});
        langItems.push_back({Item::Type::Radio, "English", kIdLangEn,
                             cfg.language == Language::English, true, {}, ""});
        items.push_back({Item::Type::Submenu, strings_.language, kIdLanguage, false, true,
                         langItems, ""});
    }

    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.reRender, kIdReload, false, true, {},
                     "Ctrl+R"});
    items.push_back({Item::Type::Action, strings_.debug, kIdDebug, false, true, {},
                     "Ctrl+D"});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.openAtLogin, kIdOpenAtLogin,
                     cfg.openAtLogin, true, {}, ""});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.feedback, kIdFeedback, false, true, {},
                     ""});
    items.push_back({Item::Type::Action, strings_.about, kIdAbout, false, true, {}, ""});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.quit, kIdQuit, false, true, {}, ""});

    tray_.UpdateMenu(items);
}

void App::OnTrayItem(const std::string& id, bool checked) {
    auto& cfg = configStore_.config();

    if (id == kIdAlwaysOnTop) {
        cfg.alwaysOnTop = checked;
        window_.SetAlwaysOnTop(checked);
    } else if (id == kIdShowTool) {
        showTool_ = checked;
        cfg.showTool = checked;
    } else if (id == kIdIgnoreMouse) {
        cfg.ignoreMouseEvents = checked;
        window_.SetIgnoreMouseEvents(checked);
    } else if (id == kIdSettings) {
        // 设置界面为独立 Qt 窗口，直接弹出。
        settingsPanel_.Toggle();
    } else if (id == kIdPrevModel) {
        SwitchModel(-1);
    } else if (id == kIdNextModel) {
        SwitchModel(1);
    } else if (id == kIdImportModel) {
        // 用 Qt 原生文件对话框选模型文件（.pmx / .vrm / .json / .zip）。
        // 压缩包会先解压到缓存目录，再自动定位其中的模型文件。
        settingsPanel_.OpenImportDialog();
    } else if (id == kIdLangZh) {
        cfg.language = Language::Chinese;
        currentLanguage_ = Language::Chinese;
        strings_ = GetStrings(currentLanguage_);
        settingsPanel_.Refresh();
        RebuildTrayMenu();
    } else if (id == kIdLangEn) {
        cfg.language = Language::English;
        currentLanguage_ = Language::English;
        strings_ = GetStrings(currentLanguage_);
        settingsPanel_.Refresh();
        RebuildTrayMenu();
    } else if (id == kIdReload) {
        // 重新渲染：重新加载当前模型以重置骨骼与表情状态。
        if (!cfg.modelPath.empty()) {
            std::string err;
            if (!LoadModel(cfg.modelPath, err)) {
                PPET_LOG_ERR("%s\n", err.c_str());
            }
        }
    } else if (id == kIdDebug) {
        PPET_LOG("Debug: desktop=%dx%d+%d+%d, tracker=%d, model=%s\n",
                    cursorTracker_.Desktop().width, cursorTracker_.Desktop().height,
                    cursorTracker_.Desktop().x, cursorTracker_.Desktop().y,
                    static_cast<int>(cursorTracker_.IsRunning()),
                    model_ ? model_->DisplayName().c_str() : strings_.noModel.c_str());
    } else if (id == kIdOpenAtLogin) {
        cfg.openAtLogin = checked;
        std::string err;
        if (!SetAutoStart(checked, err)) {
            PPET_LOG_ERR("%s\n", err.c_str());
            cfg.openAtLogin = !checked;  // 设置失败则回滚勾选状态
        }
    } else if (id == kIdFeedback) {
        std::string openError;
        if (!OpenExternalUrl("https://github.com/zenghongtu/PPet/issues", openError)) {
            PPET_LOG_ERR("%s\n", openError.c_str());
        }
    } else if (id == kIdAbout) {
        // 「关于」打开设置窗口（面板内含「关于」标签页）。
        settingsPanel_.Show();
    } else if (id == kIdQuit) {
        running_ = false;
        window_.RequestClose();
    }

    RebuildTrayMenu();
    SaveConfig();
}

void App::SwitchModel(int delta) {
    auto& cfg = configStore_.config();
    if (cfg.modelList.empty()) return;

    int idx = -1;
    for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
        if (cfg.modelList[i] == cfg.modelPath) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) idx = 0;
    else {
        idx += delta;
        if (idx < 0) idx = static_cast<int>(cfg.modelList.size()) - 1;
        if (idx >= static_cast<int>(cfg.modelList.size())) idx = 0;
    }

    const std::string path = cfg.modelList[static_cast<std::size_t>(idx)];
    std::string err;
    if (!LoadModel(path, err)) {
        PPET_LOG_ERR("%s\n", err.c_str());
        return;
    }
    SaveConfig();
}

void App::SaveConfig() {
    auto& cfg = configStore_.config();
    const Vec2 pos = window_.Position();
    const Vec2 size = window_.Size();
    cfg.windowX = static_cast<int>(pos.x);
    cfg.windowY = static_cast<int>(pos.y);
    cfg.windowWidth = static_cast<int>(size.x);
    cfg.windowHeight = static_cast<int>(size.y);

    std::string err;
    if (!configStore_.Save(configPath_, err)) {
        PPET_LOG_ERR("%s\n", err.c_str());
    }
}

RenderContext App::BuildRenderContext(float deltaTime) const {
    RenderContext ctx;
    const Vec2 size = window_.Size();
    ctx.viewportWidth = static_cast<int>(size.x);
    ctx.viewportHeight = static_cast<int>(size.y);
    ctx.deltaTime = deltaTime;
    // 渲染缩放：模型在窗口中的显示大小（>1 更大）。
    ctx.renderScale = configStore_.config().renderScale;

    const Vec2 pos = window_.Position();
    ctx.windowOrigin = pos;
    ctx.windowSize = size;

    // 模型在屏幕上的中心与半径。
    // 3D 模型按窗口中心计算；Live2D 同样以窗口中心近似。
    ctx.modelScreenCenter = Vec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    ctx.modelScreenRadius = std::max(size.x, size.y) * 0.5f;

    if (cursorTracker_.IsRunning() && cursorTracker_.HasPosition()) {
        ctx.hasGlobalCursor = true;
        ctx.globalCursor = cursorTracker_.Position();
    }

    // 【全屏追踪】把全局鼠标换算成相对模型中心的归一化方向。
    // 关键点：这里用的是整个虚拟桌面的坐标，鼠标无论在哪个显示器、
    // 哪个应用之上都能被追踪到，不再受模型窗口边界限制。
    if (ctx.hasGlobalCursor) {
        Vec2 normalized;
        GlobalCursorTracker::NormalizeAround(ctx.globalCursor, ctx.modelScreenCenter,
                                             configStore_.config().trackingRadius, &normalized);
        ctx.localCursor = normalized;
    }

    return ctx;
}

void App::ProcessInput() {
    // 按键处理：保留原项目的快捷键习惯。
    // GLFW 的键盘回调已由平台层分发，这里通过轮询补充处理。
}

void App::Run() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    lastFrameTime_ = 0.0f;

    while (running_ && !window_.ShouldClose()) {
        const auto now = clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        // 限制 dt 上限，避免窗口被拖动/挂起后出现跳变。
        const float deltaTime = std::min(dt, 0.1f);

        window_.PollEvents();
        // 维持置顶：在独占全屏游戏中窗口会被压到后面，需要周期性提升。
        window_.UpdatePlatformState();
        // Linux 托盘运行在独立线程，需要每帧把事件投递回主线程。
        tray_.PumpEvents();

        cursorTracker_.Update();

        ProcessInput();

        const RenderContext ctx = BuildRenderContext(deltaTime);
        if (model_) {
            model_->Update(ctx);
            model_->Render(ctx);
        } else {
            ClearTransparent();
        }

        // 设置界面是独立 Qt 窗口；每帧手动驱动 Qt 事件分发，
        // 与 GLFW 主循环共用同一条主线程（无需独立 Qt 线程）。
        settingsPanel_.PumpEvents();

        window_.SwapBuffers();
    }
}

void App::Shutdown() {
    cursorTracker_.Stop();
    settingsPanel_.Shutdown();
    tray_.Destroy();
    window_.Destroy();
    model_.reset();
    running_ = false;
}

}  // namespace ppet
