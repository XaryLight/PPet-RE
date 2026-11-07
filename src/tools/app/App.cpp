// PPet - C++ 移植版
// 应用主控制器实现。
#include <src/tools/app/App.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <GLFW/glfw3.h>

#include <src/tools/core/Math.hpp>
#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/render/FrameCapture.hpp>
#include <src/ui/ErrorDialog.hpp>
#include <src/tools/platform/AutoStart.hpp>
#include <src/tools/platform/OpenExternal.hpp>
#include <src/tools/render/RenderUtils.hpp>

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
constexpr const char* kIdDebugPanel = "debugPanel";
constexpr const char* kIdOpenAtLogin = "openAtLogin";
constexpr const char* kIdFeedback = "feedback";
constexpr const char* kIdAbout = "about";
constexpr const char* kIdQuit = "quit";

/// 从 URL 字符串中提取模型路径（去掉查询参数）。
std::string StripQuery(const std::string& url) {
    const std::size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q);
}

/// 由「路径 + 模型类型」生成稳定的短 ID（FNV-1a 64），
/// 供诊断面板与日志对照：同一个模型在任何机器上都是同一个 ID。
std::string MakeModelId(const std::string& path, ModelType type) {
    if (path.empty()) return {};
    std::uint64_t hash = 14695981039346656037ull;
    const std::string seed = path + "|" + std::to_string(static_cast<int>(type));
    for (unsigned char c : seed) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "PPET-%016llX",
                  static_cast<unsigned long long>(hash));
    return buf;
}

}  // namespace

App::~App() { Shutdown(); }

bool App::Initialize(std::string& error) {
    // --- 配置 ---
    configPath_ = ConfigStore::DefaultConfigPath();
    // 首次启动（配置文件不存在）时自动弹出设置面板，
    // 否则新用户面对的是一个透明的空窗口，不知道该做什么。
    const bool firstLaunch = !std::filesystem::exists(ToFsPath(configPath_));
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
        ModelLoadError err;
        // notifyUser=true：失败/隐患由 LoadModel 统一弹 Qt 提示框。
        if (!LoadModel(path, err, true)) {
            PPET_LOG_ERR("%s: %s\n", strings_.importFailed.c_str(),
                         err.ToString().c_str());
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
            ModelLoadError err;
            if (!LoadModel(list[index], err, true)) {
                PPET_LOG_ERR("%s: %s\n", strings_.modelLoadFailed.c_str(),
                             err.ToString().c_str());
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

    // --- 模型调试/诊断面板（独立 Qt 6 窗口）---
    // 与设置面板共用同一个 QApplication 和主循环事件泵，不额外开线程。
    std::string debugError;
    debugPanel_.strings = &strings_;
    if (!debugPanel_.Initialize(debugError)) {
        // 诊断界面不可用也不应阻止启动。
        PPET_LOG_WARN("%s: %s\n", strings_.debugPanel.c_str(), debugError.c_str());
    }

    // --- 模型 ---
    const auto& cfg = configStore_.config();
    if (!cfg.modelPath.empty()) {
        ModelLoadError modelError;
        // 启动时的自动加载不弹窗（避免每次开机弹）；失败信息仍写入 last.log，
        // 并可在「模型诊断」面板的「错误与警告」页查看。
        if (!LoadModel(cfg.modelPath, modelError, false)) {
            PPET_LOG_ERR("%s: %s\n", strings_.modelLoadFailed.c_str(),
                         modelError.ToString().c_str());
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
    // 设置面板与诊断面板直接用最新 strings_ 重绘文案。
    settingsPanel_.Refresh();
    debugPanel_.Refresh();
    RebuildTrayMenu();
}

std::string App::ModelCacheDirectory() const {
    // 与配置文件同级的 models 目录：Windows 下是 %APPDATA%/PPet/models。
    const std::string configDir = ParentDirectory(configPath_);
    if (configDir.empty()) return "models";
    return JoinPath(configDir, "models");
}

bool App::LoadModel(const std::string& path, std::string& error) {
    ModelLoadError detailed;
    const bool ok = LoadModel(path, detailed, false);
    error = detailed.ToString();
    return ok;
}

void App::ReportModelProblem(const ModelLoadError& error, bool showDialog) {
    if (error.ok()) return;
    if (error.warningOnly) {
        PPET_LOG_TAG_WARN("MODEL", "%s", error.ToString().c_str());
    } else {
        PPET_LOG_TAG_ERR("MODEL", "%s", error.ToString().c_str());
    }
    // ToReport() 是多行的；日志系统会把内部换行转义，仍保持「一行一条」。
    PPET_LOG_TAG_DEBUG("MODEL", "%s", error.ToReport().c_str());
    if (showDialog) {
        ShowModelErrorDialog(error, strings_, Logger::Path());
    }
}

bool App::LoadModel(const std::string& path, ModelLoadError& out, bool notifyUser) {
    out.Clear();
    lastLoadError_.Clear();

    // 所有失败走同一个出口：填结构化错误 -> 记日志 -> 按需弹窗。
    auto fail = [&](ModelErrorCategory category, const std::string& why,
                    const std::string& where) -> bool {
        out.Fail(category, why, where);
        lastLoadError_ = out;
        ReportModelProblem(out, notifyUser);
        return false;
    };

    if (path.empty()) {
        return fail(ModelErrorCategory::EmptyPath, "模型路径为空", std::string());
    }

    // 在线模型地址去掉查询参数后再判断类型（在线地址常带 ?token=xxx）。
    std::string cleanPath = StripQuery(path);
    PPET_LOG_TAG_DEBUG("MODEL", "请求加载模型：%s", path.c_str());

    // 压缩包导入：先把 zip 解压到缓存目录，再定位其中的模型文件。
    // PMX 尤其需要这一步 —— .pmx 只记录贴图路径，不内嵌图形数据。
    if (FileExtension(cleanPath) == "zip") {
        ModelPackage package;
        std::string packageError;
        if (!ExtractModelPackage(cleanPath, ModelCacheDirectory(), package, packageError)) {
            return fail(ModelErrorCategory::PackageError, packageError, cleanPath);
        }
        cleanPath = package.modelPath;
        lastPackage_ = package;
        lastFromZip_ = true;
        PPET_LOG_TAG("MODEL", "模型来自 ZIP 包：%s（%s / %zu 条目 / %zu 张贴图）",
                     package.sourceArchive.c_str(), PackageModelKindName(package.kind),
                     package.entryCount, package.textureCount);
    } else {
        lastPackage_ = ModelPackage();
        lastFromZip_ = false;
    }

    const ModelType type = DetectModelType(cleanPath);
    if (type == ModelType::Unknown) {
        return fail(ModelErrorCategory::UnsupportedFormat,
                    "无法识别的模型格式（支持 .zip / .pmx / .vrm / .model3.json / .moc3）",
                    cleanPath);
    }

    ModelPtr model = CreateModel(type);
    if (!model) {
        if (type == ModelType::Live2D) {
            return fail(ModelErrorCategory::BackendUnavailable,
                        "Live2D 模型需要编译 Cubism SDK 后端（CMake 选项 PPET_WITH_CUBISM）",
                        cleanPath);
        }
        return fail(ModelErrorCategory::BackendUnavailable, "该模型类型的后端不可用",
                    cleanPath);
    }

    const auto loadStarted = std::chrono::steady_clock::now();
    std::string rawError;
    if (!model->Load(cleanPath, rawError)) {
        out = ModelLoadError::FromRaw(rawError, cleanPath);
        out.details.push_back("模型类型：" + std::string(ModelTypeName(type)));
        out.details.push_back("解析入口：" + cleanPath);
        if (type == ModelType::Pmx) {
            out.details.push_back("PMX 只记录贴图路径、不含图形数据，请把 .pmx 与贴图目录打包成 .zip");
        }
        if (type == ModelType::Live2D) {
            out.details.push_back("Live2D 需要 .model3.json 与同目录的 .moc3 配套");
        }
        lastLoadError_ = out;
        ReportModelProblem(out, notifyUser);
        return false;
    }
    const double loadMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStarted)
            .count();

    const ModelDebugInfo info = model->DebugInfo();
    PPET_LOG_TAG("MODEL", "模型加载成功：%s（%s / %s / %.1f ms）",
                 info.displayName.c_str(), ModelTypeName(info.type),
                 info.formatVersion.c_str(), loadMs);
    PPET_LOG_TAG_DEBUG("MODEL", "规模：顶点 %zu / 三角形 %zu / 骨骼 %zu / 材质 %zu / 子网格 %zu",
                       info.vertexCount, info.triangleCount, info.boneCount,
                       info.materialCount, info.subMeshCount);

    // 引用了但磁盘上不存在的资源：PMX 单独导入时几乎必然触发。
    const std::vector<std::string> missing = model->MissingResources();
    if (!missing.empty()) {
        PPET_LOG_TAG_WARN("MODEL", "模型引用了 %zu 个不存在的资源文件：", missing.size());
        const std::size_t shown = std::min<std::size_t>(missing.size(), 5);
        for (std::size_t i = 0; i < shown; ++i) {
            PPET_LOG_TAG_WARN("MODEL", "  [%zu/%zu] %s", i + 1, missing.size(),
                              missing[i].c_str());
        }
        if (missing.size() > shown) {
            PPET_LOG_TAG_WARN("MODEL", "  ... 其余 %zu 个可在「模型诊断」面板查看",
                              missing.size() - shown);
        }
    }

    // 「加载成功但有隐患」：合并成一个 warningOnly 错误，用户主动导入时弹窗说明。
    // 这覆盖了「未找到有效骨架」「模型图片缺失」这两类最让用户困惑的情况 ——
    // 它们往往不会让 Load() 失败，但模型看起来就是不对。
    ModelLoadError warning;
    warning.warningOnly = true;
    warning.path = cleanPath;
    if (!missing.empty()) {
        warning.category = ModelErrorCategory::MissingTexture;
        warning.summary = "模型已加载，但缺少 " + std::to_string(missing.size()) +
                          " 张贴图/图片（可能白模或全身纯色）";
        warning.details = missing;
        if (type == ModelType::Pmx) {
            warning.advice = ModelErrorCategoryAdvice(ModelErrorCategory::MissingTexture);
        }
    }
    // Live2D 是二维部件管线，本来就没有骨骼，不做骨架检查。
    if (type != ModelType::Live2D && info.boneCount == 0) {
        if (warning.category == ModelErrorCategory::None) {
            warning.category = ModelErrorCategory::NoValidSkeleton;
            warning.summary = "模型已加载，但未找到有效骨架（骨骼数为 0）";
        }
        warning.details.push_back(
            "未找到有效骨架：骨骼数 0，无法做蒙皮动画与视线追踪（通常是导出时未包含骨架或文件被截断）");
    }
    if (!warning.ok()) {
        lastLoadError_ = warning;
        ReportModelProblem(warning, notifyUser);
    }

    model_ = model;
    const Vec2 size = window_.Size();
    model_->OnResize(static_cast<int>(size.x), static_cast<int>(size.y));

    auto& cfg = configStore_.config();
    // 保存「实际加载的模型文件」路径：zip 导入时这是解压后的模型文件，
    // 这样下次启动能直接加载，而不必再走一次解压。
    cfg.modelPath = cleanPath;
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
    // 「模型诊断」：打开 Qt 调试面板，显示模型部分/参数/性质/渲染状态。
    items.push_back({Item::Type::Action, strings_.debugPanel, kIdDebugPanel, false, true, {},
                     "Ctrl+Shift+D"});
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
        debugPanel_.Refresh();
        RebuildTrayMenu();
    } else if (id == kIdLangEn) {
        cfg.language = Language::English;
        currentLanguage_ = Language::English;
        strings_ = GetStrings(currentLanguage_);
        settingsPanel_.Refresh();
        debugPanel_.Refresh();
        RebuildTrayMenu();
    } else if (id == kIdReload) {
        // 重新渲染：重新加载当前模型以重置骨骼与表情状态。
        if (!cfg.modelPath.empty()) {
            ModelLoadError err;
            if (!LoadModel(cfg.modelPath, err, true)) {
                PPET_LOG_ERR("%s\n", err.ToString().c_str());
            }
        }
    } else if (id == kIdDebug) {
        const auto desktop = cursorTracker_.Desktop();
        PPET_LOG_TAG("DEBUG",
                     "桌面 %dx%d+%d+%d | 全屏追踪 %s | 视口 %dx%d | 渲染缩放 %.2f | "
                     "帧数 %llu | FPS %.1f | 当前模型 %s",
                     desktop.width, desktop.height, desktop.x, desktop.y,
                     cursorTracker_.IsRunning() ? "运行中" : "关闭",
                     static_cast<int>(window_.Size().x), static_cast<int>(window_.Size().y),
                     static_cast<double>(configStore_.config().renderScale),
                     static_cast<unsigned long long>(frameCount_),
                     static_cast<double>(frameMsSmoothed_ > 0.0f ? 1000.0f / frameMsSmoothed_
                                                                 : 0.0f),
                     model_ ? model_->DisplayName().c_str() : strings_.noModel.c_str());
        if (model_) {
            const ModelDebugInfo info = model_->DebugInfo();
            PPET_LOG_TAG("DEBUG",
                         "模型诊断：%s | %s | 顶点 %zu / 三角形 %zu / 贴图 %zu(已加载 %zu) | "
                         "缺失资源 %zu | %s",
                         info.formatVersion.c_str(), info.renderStatus.c_str(),
                         info.vertexCount, info.triangleCount, info.textureReferenced,
                         info.textureLoaded, info.missingResources.size(),
                         info.path.c_str());
        }
    } else if (id == kIdDebugPanel) {
        // 打开 Qt 诊断面板：模型的部分、参数、性质与是否正常渲染一目了然。
        debugPanel_.Toggle();
        PPET_LOG_TAG_DEBUG("DEBUG", "模型诊断面板：%s",
                           debugPanel_.IsVisible() ? "已打开" : "已关闭");
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
    ModelLoadError err;
    if (!LoadModel(path, err, true)) {
        PPET_LOG_ERR("%s\n", err.ToString().c_str());
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

        // 帧统计用真实 dt（不 clamp），指数平滑后给诊断面板算 FPS / 帧时。
        const float frameMs = dt * 1000.0f;
        frameMsSmoothed_ = frameMsSmoothed_ <= 0.0f
                               ? frameMs
                               : frameMsSmoothed_ * 0.9f + frameMs * 0.1f;
        ++frameCount_;

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

        // 诊断面板：只有在可见且到达刷新时刻（200 ms）时才采集快照。
        // IModel::DebugInfo() 会检查贴图文件是否存在，每帧做会有不必要的 IO。
        if (debugPanel_.ShouldRefresh()) {
            RuntimeStats stats;
            stats.fps = frameMsSmoothed_ > 0.0f ? 1000.0f / frameMsSmoothed_ : 0.0f;
            stats.frameMs = frameMsSmoothed_;
            stats.deltaTime = deltaTime;
            stats.frameCount = frameCount_;
            stats.viewportWidth = ctx.viewportWidth;
            stats.viewportHeight = ctx.viewportHeight;
            stats.renderScale = ctx.renderScale;
            stats.globalCursor = ctx.globalCursor;
            stats.localCursor = ctx.localCursor;
            stats.hasGlobalCursor = ctx.hasGlobalCursor;
            stats.trackerRunning = cursorTracker_.IsRunning();
            const auto desktop = cursorTracker_.Desktop();
            stats.desktopWidth = desktop.width;
            stats.desktopHeight = desktop.height;
            stats.logPath = Logger::Path();
            // 消费 GL 错误队列：0 表示本帧区间内没有 GL 错误。
            stats.glError = ConsumeGlErrors();

            ModelDebugInfo info = model_ ? model_->DebugInfo() : ModelDebugInfo();
            info.sourceArchive = lastFromZip_ ? lastPackage_.sourceArchive : std::string();
            info.glError = stats.glError;
            // 稳定 ID + 最近一次加载的详细错误（供诊断面板对照日志）。
            info.modelId = model_ ? MakeModelId(info.path, info.type) : std::string();
            if (!lastLoadError_.ok()) {
                info.errorCategory = lastLoadError_.CategoryName();
                info.errorDetails = lastLoadError_.details;
                info.errorDetails.insert(info.errorDetails.begin(),
                                         lastLoadError_.summary);
            }
            // 抓当前渲染帧给「图片渲染」页预览。仅面板可见时执行，
            // 且 App 已经先用 ConsumeGlErrors() 采集过本帧 GL 错误。
            {
                const std::string shotPath =
                    (std::filesystem::temp_directory_path() /
                     "ppet_render_preview.png").string();
                int shotW = 0;
                int shotH = 0;
                std::string capError;
                if (CaptureRenderFrame(shotPath, shotW, shotH, capError)) {
                    stats.renderPreviewPath = shotPath;
                    char note[128];
                    std::snprintf(note, sizeof(note),
                                  "渲染帧 %d x %d（面板可见时每 200 ms 抓取）",
                                  shotW, shotH);
                    stats.renderPreviewNote = note;
                } else {
                    stats.renderPreviewNote = "渲染帧抓取失败：" + capError;
                }
            }
            if (stats.glError != 0 && model_) {
                info.renderingOk = false;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "GL 错误码 %u", stats.glError);
                info.renderStatus = buf;
            }
            debugPanel_.Update(info, stats);
        }
        debugPanel_.PumpEvents();

        window_.SwapBuffers();
    }
}

void App::Shutdown() {
    cursorTracker_.Stop();
    debugPanel_.Shutdown();
    settingsPanel_.Shutdown();
    tray_.Destroy();
    window_.Destroy();
    model_.reset();
    running_ = false;
}

}  // namespace ppet
