// PPet - C++ 移植版
// 应用主控制器实现。
#include <src/tools/app/App.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <thread>

#include <GLFW/glfw3.h>

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QString>

#include <src/tools/core/Math.hpp>
#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelPackage.hpp>
#include <src/tools/core/ModelNaming.hpp>
#include <src/tools/core/Stability.hpp>
#include <src/tools/core/TrackingPolicy.hpp>
#include <src/tools/plugin/BuiltinPlugins.hpp>
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
constexpr const char* kIdResetPosition = "resetPosition";
constexpr const char* kIdMoveWindow = "moveWindow";
constexpr const char* kIdSettings = "settings";
constexpr const char* kIdPrevModel = "prevModel";
constexpr const char* kIdNextModel = "nextModel";
constexpr const char* kIdImportModel = "importModel";
constexpr const char* kIdLanguage = "language";
constexpr const char* kIdLangZh = "lang.zh";
constexpr const char* kIdLangEn = "lang.en";
constexpr const char* kIdReload = "reload";
// 「调试」(原 Ctrl+D，纯写日志) 与「模型分析」(原 Ctrl+Shift+D，Qt 面板) 已合并：
// 只保留 kIdDebugPanel 一个入口，打开诊断面板的同时把诊断摘要写入日志。
constexpr const char* kIdDebugPanel = "debugPanel";
constexpr const char* kIdOpenAtLogin = "openAtLogin";
constexpr const char* kIdFeedback = "feedback";
constexpr const char* kIdAbout = "about";
constexpr const char* kIdQuit = "quit";
// 表情与语言功能入口
constexpr const char* kIdExpressions = "expressions";  ///< 「表情」子菜单
constexpr const char* kIdSayPhrase = "sayPhrase";      ///< 「说一句」
constexpr const char* kIdBubble = "bubble";            ///< 台词气泡开关
constexpr const char* kIdRandomExpr = "randomExpr";    ///< 待机随机表情开关
constexpr const char* kIdRandomExprNow = "randomExprNow"; ///< 立即随机一个表情
/// expr:<presetId> 形式的菜单项：触发某个表情预设。
constexpr const char* kExprPrefix = "expr:";

/// 不打开诊断面板时，每隔多少帧做一次「帧缓冲是否有像素」的轻量自检。
/// 60 FPS 下约 2 秒一次；每次只读回一帧 RGBA，开销远小于一次贴图上传。
constexpr std::uint64_t kEvidenceProbeFrames = 120;

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

/// 表情预设 id -> 本地化显示名。
///
/// 预设 id 是 ExpressionDirector 的固定契约（neutral/happy/…），
/// 显示名走 i18n，未知 id 原样返回（自定义预设也能正常显示）。
std::string ExpressionPresetLabel(const Strings& s, const std::string& id) {
    if (id == "neutral") return s.expressionNeutral;
    if (id == "happy") return s.expressionHappy;
    if (id == "relaxed") return s.expressionRelaxed;
    if (id == "angry") return s.expressionAngry;
    if (id == "sad") return s.expressionSad;
    if (id == "surprised") return s.expressionSurprised;
    return id;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

App::~App() { Shutdown(); }

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

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

    // 运行时兼容选项：命令行 --safe-mode / --max-fps= 优先于配置文件，
    // 但只作用于本次运行，不写回磁盘。
    safeMode_ = hasSafeModeOverride_ ? safeModeOverride_
                                    : configStore_.config().safeMode;
    configuredMaxFps_ = hasMaxFpsOverride_ ? maxFpsOverride_
                                          : configStore_.config().maxFps;
    if (safeMode_) {
        PPET_LOG("已启用兼容模式（--safe-mode）：帧率上限 30、跳过 glReadPixels "
                 "帧读回自检，用于排查第三方 overlay 注入导致的卡顿 / 崩溃");
    }

    // 虚拟桌面几何要尽早取到：加载配置、托盘重置、拖动结束都靠它把窗口钳制在
    // 可见范围内。追踪器不一定启动（fullscreenTracking 可能关闭），所以这里显式
    // 刷新一次，不能依赖 Start() 里的那次。
    cursorTracker_.RefreshDesktop();

    // --- 插件系统（需求 6）---
    // 记录启动时刻（给插件事件打时间戳），注入配置快照，注册内置插件。
    appStartTime_ = std::chrono::steady_clock::now();
    lastPluginTick_ = appStartTime_;
    pluginManager_.SetConfigSource(&configStore_.config());
    pluginManager_.Register(std::make_shared<plugin::GreeterPlugin>());
    pluginManager_.Register(std::make_shared<plugin::CounterPlugin>());
    EmitPluginEvent(plugin::PluginEventKind::AppStart);
    PPET_LOG_TAG("PLUGIN", "插件系统就绪：已注册 %zu 个内置插件",
                 pluginManager_.List().size());

    // --- 窗口 ---
    WindowOptions opts;
    opts.width = configStore_.config().windowWidth;
    opts.height = configStore_.config().windowHeight;
    // 旧版 Windows 上 Window::Position() 曾返回约 2 倍的坐标，配置里可能已经存了
    // 把窗口顶出屏幕的坏值。这里统一钳制，保证每次启动都能看见窗口。
    const IntRect safePos = ClampRectInside(
        IntRect{configStore_.config().windowX, configStore_.config().windowY,
                opts.width, opts.height},
        DesktopRect());
    opts.x = safePos.x;
    opts.y = safePos.y;
    opts.title = "PPet";
    opts.alwaysOnTop = configStore_.config().alwaysOnTop;
    opts.transparent = true;
    opts.resizable = false;
    opts.skipTaskbar = true;

    if (!window_.Create(opts, error)) return false;

    // --- 输入回调 ---
    window_.onMouseButton = [this](int button, int action) {
        if (button != GLFW_MOUSE_BUTTON_LEFT) return;

        if (action == GLFW_PRESS) {
            // 点击穿透开启时默认收不到鼠标事件；「移动窗口」模式会临时解除穿透，
            // 让平时穿透的窗口也能被拖回来。
            if (configStore_.config().ignoreMouseEvents && !moveMode_) return;
            double cx = 0.0;
            double cy = 0.0;
            glfwGetCursorPos(window_.Handle(), &cx, &cy);
            // A = 抓取点在窗口内的偏移，之后 W' = W + L - A = G - A。
            dragGrabX_ = cx;
            dragGrabY_ = cy;
            dragging_ = true;
        } else if (action == GLFW_RELEASE) {
            if (!dragging_) return;
            dragging_ = false;
            // 松手时兜底钳制，避免最后一次移动把窗口带出可视范围。
            ClampWindowToDesktop();
            if (windowMovedByUser_) {
                windowMovedByUser_ = false;
                SaveConfig();
            } else if (!moveMode_ && model_) {
                // 按下后没有拖动 = 一次点击：交给模型做互动（Live2D 按命中区
                // 触发 TapHead/TapBody 等动作）。移动窗口模式下只拖动，不触发。
                // 归一化坐标：dragGrabX_/Y_ 是按下时光标在窗口内的像素位置。
                const Vec2 size = window_.Size();
                if (size.x > 0.0f && size.y > 0.0f) {
                    const float nx = std::clamp(
                        static_cast<float>(dragGrabX_ / size.x), 0.0f, 1.0f);
                    const float ny = std::clamp(
                        static_cast<float>(dragGrabY_ / size.y), 0.0f, 1.0f);
                    model_->OnInteract(nx, ny);
                    // 需求 6：广播「用户互动」事件。区域判定复用 Model.hpp 的
                    // RegionFromPoint（与 NormalizedFromRegion 互为反查），
                    // 保证各处「坐标 → 分区」口径一致。
                    const std::string region = RegionFromPoint(nx, ny);
                    EmitPluginEvent(plugin::PluginEventKind::Interaction,
                                    std::string(), {{"region", region}});
                    // 点击角色时来一句问候，配合口型让桌宠更有回应感。
                    SayState(agent::AgentStateKind::Greeting, 2);
                }
            }
        }
    };

    window_.onMouseMove = [this](double xpos, double ypos) {
        // 记录窗口内鼠标位置：作为「全屏追踪不可用时」的视线追踪兜底。
        // 桌宠窗口通常开启了点击穿透，全局钩子拿不到位置，但窗口内的
        // 鼠标移动事件仍然会送达——至少让模型能看向窗口内的鼠标。
        windowCursor_ = Vec2(static_cast<float>(xpos), static_cast<float>(ypos));
        hasWindowCursor_ = true;

        if (!dragging_) return;
        // 绝对定位：记 W = 窗口左上角、L = 光标在窗口内坐标，则全局光标
        // G = W + L 恒成立（窗口移动会等量反向改变 L）。令 W' = G - A，
        // 窗口精确跟随光标，且不会因为 W 变化反过来改变 L 而产生正反馈漂移。
        //
        // 旧实现用「L 的增量」去移动 W，且按下时把偏移清成 0：首次移动窗口就会
        // 跳一个窗口内坐标的距离，随后反复震荡、越拖越远，最终飞出桌面。
        const Vec2 cur = window_.Position();
        const double gx = static_cast<double>(cur.x) + xpos;
        const double gy = static_cast<double>(cur.y) + ypos;
        window_.SetPosition(static_cast<int>(std::lround(gx - dragGrabX_)),
                            static_cast<int>(std::lround(gy - dragGrabY_)));
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
            return;
        }

        auto& cfg = configStore_.config();
        // 【添加模型后可以改名】GUI 导入时可选弹窗指定名字，预填默认名
        // （文件名去后缀）。取消/留空就用默认名，重名自动标注序号。
        // 脚本模式（--command / --console）不弹窗，避免阻塞无人值守运行。
        if (cfg.askModelNameOnImport && consoleScript_.empty() && !consoleForce_ &&
            QApplication::instance() != nullptr) {
            const QString defaultName =
                QString::fromStdString(model_naming::FileStem(path));
            bool accepted = false;
            const QString chosen = QInputDialog::getText(
                nullptr, QString::fromUtf8("模型名称"),
                QString::fromUtf8("给新模型起个名字（留空则使用默认名）："),
                QLineEdit::Normal, defaultName, &accepted);
            if (accepted && !chosen.isEmpty()) {
                cfg.modelNames = model_naming::ResolveNames(cfg.modelList, cfg.modelNames);
                for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
                    if (cfg.modelList[i] != cfg.modelPath) continue;
                    std::vector<std::string> others;
                    for (std::size_t j = 0; j < cfg.modelNames.size(); ++j) {
                        if (j != i) others.push_back(cfg.modelNames[j]);
                    }
                    cfg.modelNames[i] =
                        model_naming::UniqueName(chosen.toStdString(), others);
                    break;
                }
            }
        }
        SaveConfig();
        RebuildTrayMenu();
    };
    settingsPanel_.onRemoveModel = [this](int index) {
        auto& cfg = configStore_.config();
        if (index >= 0 && index < static_cast<int>(cfg.modelList.size())) {
            cfg.modelList.erase(cfg.modelList.begin() + index);
            // 名字列表必须同步删除，否则后续模型会显示成别人的名字。
            if (index < static_cast<int>(cfg.modelNames.size())) {
                cfg.modelNames.erase(cfg.modelNames.begin() + index);
            }
            SaveConfig();
            RebuildTrayMenu();
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
        // 保留当前模型与模型列表/名字，其余配置恢复默认。
        const AppConfig previous = configStore_.config();
        configStore_.config() = AppConfig();
        configStore_.config().modelPath = previous.modelPath;
        configStore_.config().modelList = previous.modelList;
        configStore_.config().modelNames = previous.modelNames;
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

    // --- 台词气泡窗口（独立 Qt 窗口，默认不置顶）---
    // 跟随桌宠窗口上方显示当前台词；托盘「台词气泡」可开关。
    std::string bubbleError;
    bubble_ = new BubbleWidget();
    if (!bubble_->Initialize(bubbleError)) {
        // 气泡不可用不应阻止启动（无气泡只是看不到台词文字）。
        PPET_LOG_WARN("%s: %s\n", strings_.bubble.c_str(), bubbleError.c_str());
        delete bubble_;
        bubble_ = nullptr;
    }

    // 语言系统：台词库已默认构造；把指针交给 SpeechController，
    // 否则 SayState 因 book_ 为空而无法抽台词。
    speech_.SetPhraseBook(&phraseBook_);

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

    // 交互式控制台：后台线程读 stdin，命令在主循环 ProcessInput() 里执行。
    SetupConsole();

    running_ = true;
    return true;
}

void App::EmitPlugin(const plugin::PluginEvent& event) {
    pluginManager_.Emit(event);
}

void App::EmitPluginEvent(plugin::PluginEventKind kind, std::string source,
                          std::map<std::string, std::string> payload) {
    plugin::PluginEvent event;
    event.kind = kind;
    event.source = std::move(source);
    event.payload = std::move(payload);
    const auto now = std::chrono::steady_clock::now();
    event.timestamp =
        std::chrono::duration<double>(now - appStartTime_).count();
    pluginManager_.Emit(event);
}

// ---------------------------------------------------------------------------
// 配置应用与语言
// ---------------------------------------------------------------------------

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

    // 需求 7 修复：把「最大偏转角度」配置实时回写到模型。
    // 此前该值从未落地到模型，拖动滑块毫无反应、且模型永远停在默认 30°（偏大）。
    // 钳制统一走 TrackingPolicy::ClampTrackingAngle（[0,90]），模型内部是第二道防线。
    if (model_) model_->SetMaxTrackingAngle(ClampTrackingAngle(cfg.maxTrackingAngle));

    // 需求 6：广播「配置变更」事件（含本次变化的关键字段），插件可据此响应。
    EmitPluginEvent(plugin::PluginEventKind::ConfigChanged, std::string(),
                    {{"fullscreenTracking", cfg.fullscreenTracking ? "1" : "0"},
                     {"maxTrackingAngle", std::to_string(cfg.maxTrackingAngle)},
                     {"trackingRadius", std::to_string(cfg.trackingRadius)}});
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

// ---------------------------------------------------------------------------
// 语言功能（台词 / 气泡 / 口型）
// ---------------------------------------------------------------------------

void App::SayState(agent::AgentStateKind kind, int priority) {
    lastSpeechKind_ = kind;
    speech_.SayState(kind, priority);
}

void App::TickDialogue(float dt) {
    if (!model_) return;

    // 口型：情绪跟随最近一次主动触发的台词（待机碎碎念为 Idle，嘴形偏中性）；
    // 说话强度跟随「是否正在说话」，让开口度随音节起伏而不是匀速张嘴。
    lipSync_.SetEmotion(lastSpeechKind_);
    lipSync_.SetIntensity(speech_.IsSpeaking() ? 1.0f : 0.0f);
    lipSync_.Tick(dt);
    // 气泡状态机：bubbleEnabled_ 同时控制「是否允许待机碎碎念」与「气泡可见性」。
    speech_.Tick(dt, bubbleEnabled_);

    // 口型写入模型：
    //   - Live2D 走 SetLipSync（ParamMouthOpenY / ParamMouthForm）；
    //   - PMX / VRM 走元音 morph 叠加层（SetExpressionWeights 的 aa/ih/ou/ee/oh）。
    const float open = lipSync_.MouthOpen();
    const float form = lipSync_.MouthForm();
    const agent::LipSync::Vowels& vowels = lipSync_.GetVowels();
    model_->SetLipSync(open, form);
    model_->SetExpressionWeights({
        {"aa", vowels.aa},
        {"ih", vowels.ih},
        {"ou", vowels.ou},
        {"ee", vowels.ee},
        {"oh", vowels.oh},
    });

    // 气泡：把当前台词推给气泡窗口，并让它跟随桌宠窗口上方。
    const std::string text = speech_.CurrentText();
    currentPhrase_ = text;
    if (bubble_) {
        bubble_->SetText(text);
        // 显隐由「有台词 + 托盘开关」共同决定：
        // 关掉气泡后桌宠照常动嘴、台词照常推进，只是不显示文字。
        bubble_->SetVisible(bubbleEnabled_ && !text.empty());
        if (!text.empty()) {
            const Vec2 size = window_.Size();
            const Vec2 pos = window_.Position();
            // 【气泡随渲染窗口缩放】窗口变宽后，固定 280px 的气泡会显得又小又挤。
            // 最大宽度按窗口宽度按比例给（并留出 15% 余量），字号同步放大；
            // 位置用气泡实际宽高居中、贴在窗口顶边上方，并钳制在虚拟桌面内。
            const int bubbleMaxW =
                static_cast<int>(std::clamp(size.x * 1.15f, 160.0f, 720.0f));
            bubble_->SetMaxWidth(bubbleMaxW);
            const int bubbleW = bubble_->Width();
            const int bubbleH = bubble_->Height();
            const int bx = static_cast<int>(pos.x + size.x * 0.5f - bubbleW * 0.5f);
            const IntRect desk = DesktopRect();
            const int by = std::max(desk.y + 4, static_cast<int>(pos.y) - bubbleH - 8);
            bubble_->MoveTo(bx, by);
        }
    }
}

// ---------------------------------------------------------------------------
// 模型生命周期（加载 / 错误报告 / 切换）
// ---------------------------------------------------------------------------

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
    auto& cfg = configStore_.config();
    model_->SetArmRestDrop(cfg.armRestDropDeg);
    // 需求 7：加载模型后同样把最大偏转角度落到模型，保证「换模型」后偏转设置不丢。
    model_->SetMaxTrackingAngle(ClampTrackingAngle(cfg.maxTrackingAngle));

    // 需求 6：广播「模型加载」事件，插件可据此做初始化 / 打招呼。
    EmitPluginEvent(plugin::PluginEventKind::ModelLoaded, cleanPath);

    // 【渲染窗口按实际帧宽高比自适应】
    // 3D 后端用包围盒 X/Y，Live2D 用 Cubism 画布尺寸。横向模型（宽 > 高）
    // 在固定的竖窗口里会被左右截断，或被迫整体缩小到看不清；按内容宽高比
    // 调整窗口后，模型既完整又占满画面。
    if (cfg.autoFitWindow) {
        const float aspect = model_->PreferredFrameAspect();
        if (aspect > 0.05f && aspect < 20.0f) {
            const Vec2 before = window_.Size();
            const IntRect desk = DesktopRect();
            const int maxW = std::max(160, desk.width - 24);
            const int maxH = std::max(200, desk.height - 24);
            int newH = std::clamp(static_cast<int>(before.y), 200, std::min(2000, maxH));
            int newW = std::clamp(static_cast<int>(static_cast<float>(newH) * aspect * 1.08f),
                                  160, maxW);
            // 宽度被桌面限制时等比压低高度，保持宽高比不失真。
            if (static_cast<float>(newW) / static_cast<float>(newH) < aspect) {
                newH = std::max(160, static_cast<int>(static_cast<float>(newW) / aspect));
            }
            const int dw = newW > static_cast<int>(before.x)
                               ? newW - static_cast<int>(before.x)
                               : static_cast<int>(before.x) - newW;
            const int dh = newH > static_cast<int>(before.y)
                               ? newH - static_cast<int>(before.y)
                               : static_cast<int>(before.y) - newH;
            if (dw > 8 || dh > 8) {
                window_.SetSize(newW, newH);
                const Vec2 after = window_.Size();
                cfg.windowWidth = static_cast<int>(after.x);
                cfg.windowHeight = static_cast<int>(after.y);
                ClampWindowToDesktop();
                PPET_LOG_TAG("WINDOW", "按模型帧宽高比 %.2f 调整渲染窗口：%dx%d -> %dx%d",
                             static_cast<double>(aspect), static_cast<int>(before.x),
                             static_cast<int>(before.y), cfg.windowWidth, cfg.windowHeight);
            }
        }
    }

    const Vec2 size = window_.Size();
    model_->OnResize(static_cast<int>(size.x), static_cast<int>(size.y));

    // 保存「实际加载的模型文件」路径：zip 导入时这是解压后的模型文件，
    // 这样下次启动能直接加载，而不必再走一次解压。
    cfg.modelPath = cleanPath;
    if (std::find(cfg.modelList.begin(), cfg.modelList.end(), cleanPath) ==
        cfg.modelList.end()) {
        cfg.modelList.push_back(cleanPath);
        // 名字列表留一个空位：Save() 时由 ModelNaming 自动补默认名并去重。
        cfg.modelNames.resize(cfg.modelList.size());
    }
    return true;
}

// ---------------------------------------------------------------------------
// 托盘菜单
// ---------------------------------------------------------------------------

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
    // 窗口位置控制：透明无边框窗口一旦跑出桌面就没有可点的地方，只能靠托盘救回。
    items.push_back({Item::Type::Action, strings_.resetPosition, kIdResetPosition,
                     false, true, {}, ""});
    items.push_back({Item::Type::Checkbox, strings_.moveWindow, kIdMoveWindow,
                     moveMode_, true, {}, ""});
    items.push_back({Item::Type::Separator, "", "", false, true, {}, ""});
    items.push_back({Item::Type::Action, strings_.prevModel, kIdPrevModel, false, true, {},
                     "Ctrl+P"});
    items.push_back({Item::Type::Action, strings_.nextModel, kIdNextModel, false, true, {},
                     "Ctrl+N"});
    // 导入模型：当前没有独立设置窗口，这里直接用系统文件对话框加载，
    // 让桌宠在透明窗口里至少能显示出来（否则窗口内容为空）。
    items.push_back({Item::Type::Action, strings_.importModel, kIdImportModel, false, true, {},
                     "Ctrl+O"});

    // 表情子菜单：列出模型支持的全部表情预设（经 model_->ExpressionPresetIds()）。
    // 模型无表情或尚未加载时，放一条禁用提示。
    {
        std::vector<Item> exprItems;
        if (model_) {
            const std::vector<std::string> presetIds = model_->ExpressionPresetIds();
            if (presetIds.empty()) {
                exprItems.push_back({Item::Type::Action, strings_.noExpressions,
                                     kIdExpressions, false, false, {}, ""});
            } else {
                for (const std::string& id : presetIds) {
                    exprItems.push_back({Item::Type::Action,
                                         ExpressionPresetLabel(strings_, id),
                                         std::string(kExprPrefix) + id, false, true, {}, ""});
                }
            }
        } else {
            exprItems.push_back({Item::Type::Action, strings_.noExpressions,
                                 kIdExpressions, false, false, {}, ""});
        }
        items.push_back({Item::Type::Submenu, strings_.expressionMenu, kIdExpressions,
                         false, true, exprItems, ""});
    }
    // 待机随机表情：桌宠每隔 8~20 秒自己换一个表情（对齐 VRM-VPet 的随机表情）。
    items.push_back({Item::Type::Checkbox, strings_.randomExpression, kIdRandomExpr,
                     model_ && model_->RandomIdleExpression(), true, {}, ""});
    // 立即随机一个表情：不等下一次自动触发，立刻抽一个。
    items.push_back({Item::Type::Action, strings_.randomExpressionNow, kIdRandomExprNow,
                     false, true, {}, ""});
    // 说一句：随机抽一条台词（含问候/思考/工作等状态），供用户主动触发。
    items.push_back({Item::Type::Action, strings_.sayPhrase, kIdSayPhrase, false, true, {},
                     ""});
    // 台词气泡：开关（默认开启；关闭后桌宠照常动嘴，只是不显示文字气泡）。
    items.push_back({Item::Type::Checkbox, strings_.bubble, kIdBubble, bubbleEnabled_, true, {},
                     ""});

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
    // 「模型诊断」= 合并后的唯一调试/分析入口：打开 Qt 诊断面板 +
    // 把桌面/追踪/模型诊断摘要写入 last.log（保留原「调试」的日志能力）。
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
        // 移动模式期间保持可交互，退出移动模式后再按配置恢复。
        window_.SetIgnoreMouseEvents(moveMode_ ? false : checked);
    } else if (id == kIdResetPosition) {
        ResetWindowPosition();
    } else if (id == kIdMoveWindow) {
        moveMode_ = checked;
        // 移动模式只是临时解除穿透：退出时按「当前配置」恢复，
        // 不用进入时的快照，避免覆盖用户在模式期间改过的设置。
        window_.SetIgnoreMouseEvents(moveMode_ ? false : cfg.ignoreMouseEvents);
        if (moveMode_) {
            PPET_LOG_TAG("WINDOW",
                         "移动窗口模式已开启：按住窗口拖动即可移动；再次点击托盘菜单结束");
        } else {
            ClampWindowToDesktop();
            SaveConfig();
            PPET_LOG_TAG("WINDOW", "移动窗口模式已结束，已恢复点击穿透并保存位置");
        }
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
    } else if (id.rfind(kExprPrefix, 0) == 0) {
        // 表情预设：id 形如 "expr:<presetId>"。
        if (model_) {
            const std::string presetId = id.substr(std::strlen(kExprPrefix));
            if (model_->SetExpressionPreset(presetId)) {
                PPET_LOG_TAG("EXPRESSION", "切换表情预设：%s", presetId.c_str());
            } else {
                PPET_LOG_TAG("EXPRESSION", "表情预设不可用：%s", presetId.c_str());
            }
        }
    } else if (id == kIdRandomExpr) {
        // 待机随机表情开关。
        if (model_) {
            const bool enabled = !model_->RandomIdleExpression();
            model_->SetRandomIdleExpression(enabled);
            PPET_LOG_TAG("DIALOGUE", "待机随机表情：%s", enabled ? "开" : "关");
            RebuildTrayMenu();
        }
    } else if (id == kIdRandomExprNow) {
        if (model_ && model_->TriggerRandomExpression()) {
            PPET_LOG_TAG("DIALOGUE", "手动随机一个表情");
        }
    } else if (id == kIdSayPhrase) {
        // 随机挑一种状态说一句（比固定档位更鲜活）。
        // 用固定种子：同一会话内顺序可复现，便于排查问题时对照日志。
        static const agent::AgentStateKind kKinds[] = {
            agent::AgentStateKind::Idle,     agent::AgentStateKind::Thinking,
            agent::AgentStateKind::Working,  agent::AgentStateKind::Waiting,
            agent::AgentStateKind::Success,  agent::AgentStateKind::Error,
            agent::AgentStateKind::Greeting,
        };
        static std::mt19937 rng(20240101u);
        std::uniform_int_distribution<std::size_t> dist(
            0, sizeof(kKinds) / sizeof(kKinds[0]) - 1);
        const agent::AgentStateKind kind = kKinds[dist(rng)];
        SayState(kind, 1);
        PPET_LOG_TAG("DIALOGUE", "手动说一句：%s", agent::AgentStateName(kind));
    } else if (id == kIdBubble) {
        bubbleEnabled_ = checked;
        PPET_LOG_TAG("DIALOGUE", "台词气泡：%s", checked ? "开启" : "关闭");
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
    } else if (id == kIdDebugPanel) {
        // 合并后的唯一「模型诊断」入口：打开 Qt 诊断面板（模型部分/参数/性质/
        // 渲染状态/动作系统一目了然），同时把关键诊断摘要写入 last.log，
        // 保留原「调试 Ctrl+D」无界面场景下也能排查的能力。
        debugPanel_.Toggle();
        PPET_LOG_TAG_DEBUG("DEBUG", "模型诊断面板：%s",
                           debugPanel_.IsVisible() ? "已打开" : "已关闭");
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
                         "缺失资源 %zu | 动作 | %s | %s",
                         info.formatVersion.c_str(), info.renderStatus.c_str(),
                         info.vertexCount, info.triangleCount, info.textureReferenced,
                         info.textureLoaded, info.missingResources.size(),
                         info.motionStatus.empty() ? "-" : info.motionStatus.c_str(),
                         info.path.c_str());
        }
    } else if (id == kIdOpenAtLogin) {
        cfg.openAtLogin = checked;
        std::string err;
        if (!SetAutoStart(checked, err)) {
            PPET_LOG_ERR("%s\n", err.c_str());
            cfg.openAtLogin = !checked;  // 设置失败则回滚勾选状态
        }
    } else if (id == kIdFeedback) {
        std::string openError;
        if (!OpenExternalUrl("https://github.com/XaryLight/PPet-RE/issues", openError)) {
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

// ---------------------------------------------------------------------------
// 配置保存与窗口位置
// ---------------------------------------------------------------------------

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

IntRect App::DesktopRect() const {
    const VirtualDesktop d = cursorTracker_.Desktop();
    return IntRect{d.x, d.y, d.width, d.height};
}

void App::ClampWindowToDesktop() {
    const Vec2 pos = window_.Position();
    const Vec2 size = window_.Size();
    const IntRect before{static_cast<int>(pos.x), static_cast<int>(pos.y),
                         static_cast<int>(size.x), static_cast<int>(size.y)};
    const IntRect after = ClampRectInside(before, DesktopRect());
    if (after.x == before.x && after.y == before.y) return;

    window_.SetPosition(after.x, after.y);
    PPET_LOG_TAG("WINDOW", "窗口位置已钳制回可见区域：%d,%d -> %d,%d",
                 before.x, before.y, after.x, after.y);
}

void App::ResetWindowPosition() {
    const Vec2 size = window_.Size();
    // 用户指定的 (0,0) 即主显示器左上角；在虚拟桌面坐标系里它恒为有效可见点
    // （多显示器时虚拟桌面左边界可能为负，但 (0,0) 仍落在主屏内）。
    // 仍过一次钳制，兼容「窗口比屏幕还大」等极端配置。
    IntRect target{0, 0, static_cast<int>(size.x), static_cast<int>(size.y)};
    target = ClampRectInside(target, DesktopRect());
    window_.SetPosition(target.x, target.y);
    SaveConfig();
    PPET_LOG_TAG("WINDOW", "窗口位置已重置到 (%d,%d)（主显示器左上角）",
                 target.x, target.y);
}

// ---------------------------------------------------------------------------
// 渲染上下文与帧证据
// ---------------------------------------------------------------------------

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

    // 【光标位置与窗口是否接收鼠标事件无关】
    // 点击穿透开启后窗口收不到任何 GLFW 鼠标事件，全局钩子也可能因为
    // fullscreenTracking 关闭 / 权限不足而没启动。此时直接向操作系统查询
    // 光标在整个虚拟桌面上的绝对坐标：GetCursorPos / XQueryPointer /
    // NSEvent.mouseLocation 不受窗口边界、穿透设置与其它应用遮挡影响。
    // 这样「忽略鼠标点击」「鼠标不在桌宠窗口上」时视线追踪依然有效。
    Vec2 cursor;
    bool hasCursor = false;
    // 需求 7 修复：全屏追踪关闭后，不再查询全局光标，避免「开关失效、关了还跟」。
    // 由纯函数 ResolveTrackingSource 决定本帧用哪种光标源（更易单测）：
    //   - 全屏追踪开启 -> Global（OS 级全局坐标）
    //   - 全屏追踪关闭且有窗口内光标 -> Window（鼠标在桌宠窗口内时跟随）
    //   - 其它 -> None（不驱动视线）
    const bool fullscreen = configStore_.config().fullscreenTracking;
    const TrackingSource trackingSource = ResolveTrackingSource(fullscreen, hasWindowCursor_);
    if (trackingSource == TrackingSource::Global) {
        if (cursorTracker_.IsRunning() && cursorTracker_.HasPosition()) {
            cursor = cursorTracker_.Position();
            hasCursor = true;
        } else if (GlobalCursorTracker::QueryCursor(cursor)) {
            hasCursor = true;
        }
    }

    if (hasCursor) {
        ctx.hasGlobalCursor = true;
        ctx.globalCursor = cursor;
        const float radius = configStore_.config().trackingRadius;
        GlobalCursorTracker::NormalizeAround(cursor, ctx.modelScreenCenter, radius,
                                             &ctx.localCursor);
    } else if (hasWindowCursor_) {
        // 【兜底】全局追踪不可用（权限不足 / 被关闭 / 平台不支持）时，
        // 退化为「窗口内追踪」：鼠标在桌宠窗口内移动时模型照样看向它。
        // 换算基准是窗口中心，半径取窗口对角线的一半，
        // 于是鼠标在窗口边缘时归一化坐标约为 ±1。
        ctx.windowCursor = windowCursor_;
        ctx.hasWindowCursor = true;
        const float radius = std::max(1.0f, std::max(size.x, size.y) * 0.5f);
        ctx.localCursor = Vec2((windowCursor_.x - size.x * 0.5f) / radius,
                               -(windowCursor_.y - size.y * 0.5f) / radius);
        ctx.localCursor.x = std::clamp(ctx.localCursor.x, -1.0f, 1.0f);
        ctx.localCursor.y = std::clamp(ctx.localCursor.y, -1.0f, 1.0f);
    }

    return ctx;
}

void App::CollectFrameEvidence(bool wantPreview) {
    std::string shotPath;
    if (wantPreview) {
        shotPath =
            (std::filesystem::temp_directory_path() / "ppet_render_preview.png").string();
    }
    int width = 0;
    int height = 0;
    std::string error;
    FramePixelStats stats;
    // pngPath 为空时只统计不写图；抓帧内部会清掉 glReadPixels 自身产生的
    // GL 错误，本帧真正的错误已在调用前由 ConsumeGlErrors() 采集完毕。
    if (!CaptureRenderFrame(shotPath, width, height, error, &stats)) {
        lastEvidenceError_ = error;
        if (wantPreview) lastPreviewPath_.clear();
        PPET_LOG_TAG_DEBUG("RENDER", "帧像素证据采集失败：%s", error.c_str());
        return;
    }

    lastEvidenceFrame_ = frameCount_;
    hasFrameEvidence_ = true;
    lastFrameStats_ = stats;
    lastEvidenceError_.clear();
    if (wantPreview) lastPreviewPath_ = shotPath;

    // 只在健康状态翻转时记一条，避免每 2 秒刷一行日志。
    const bool healthy = stats.coveragePercent > 0.0;
    if (healthy == lastEvidenceLoggedHealthy_) return;
    lastEvidenceLoggedHealthy_ = healthy;
    if (healthy) {
        PPET_LOG_TAG("RENDER", "渲染像素证据恢复正常：%lld / %lld 非透明像素（%.2f%%）",
                     stats.nonTransparent, stats.totalPixels, stats.coveragePercent);
    } else {
        PPET_LOG_TAG_ERR("RENDER",
                         "渲染像素证据异常：帧缓冲 %d x %d 内 0 个非透明像素，"
                         "模型虽在绘制但未出图 —— 检查每帧清屏、glDepthMask 与视口",
                         stats.width, stats.height);
    }
}

// ---------------------------------------------------------------------------
// 主循环与退出
// ---------------------------------------------------------------------------

void App::ProcessInput() {
    // 按键处理：保留原项目的快捷键习惯。
    // GLFW 的键盘回调已由平台层分发，这里通过轮询补充处理。
    //
    // 控制台命令必须在主线程执行（要访问模型 / 配置 / Qt 气泡），
    // 后台线程只负责把 stdin 的行放进队列，这里取出来跑。
    console_.Poll();
}

void App::Run() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    lastFrameTime_ = 0.0f;

    while (running_ && !window_.ShouldClose()) {
        const auto now = clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        // 限制 dt 上限，避免窗口被拖动/挂起后出现跳变；
        // 同时把 NaN / 负数 / Inf 收敛掉，防止长期运行中出现一次坏帧污染姿态。
        const float deltaTime = stability::ClampDelta(dt);

        // 帧统计用真实 dt（不 clamp），指数平滑后给诊断面板算 FPS / 帧时。
        const float frameMs = dt * 1000.0f;
        frameMsSmoothed_ = frameMsSmoothed_ <= 0.0f
                               ? frameMs
                               : frameMsSmoothed_ * 0.9f + frameMs * 0.1f;
        // 饱和递增：即使极端情况下到达 uint64 上限也不回绕，
        // 否则「每 N 帧自检」的差值判断会瞬间变成天文数字。
        frameCount_ = stability::SaturatingIncrement(frameCount_);

        // 需求 6：节流后的「帧心跳」事件（每秒一次），供插件做周期性逻辑，
        // 避免每帧广播造成刷屏。fps 由平滑帧时换算。
        {
            const auto tickNow = clock::now();
            if (std::chrono::duration<double>(tickNow - lastPluginTick_).count() >= 1.0) {
                lastPluginTick_ = tickNow;
                const float fps = frameMsSmoothed_ > 0.0f ? (1000.0f / frameMsSmoothed_) : 0.0f;
                EmitPluginEvent(plugin::PluginEventKind::FrameTick, std::string(),
                                {{"fps", std::to_string(static_cast<int>(fps))},
                                 {"frame", std::to_string(frameCount_)}});
            }
        }

        window_.PollEvents();
        // 维持置顶：在独占全屏游戏中窗口会被压到后面，需要周期性提升。
        window_.UpdatePlatformState();
        // Linux 托盘运行在独立线程，需要每帧把事件投递回主线程。
        tray_.PumpEvents();

        cursorTracker_.Update();

        ProcessInput();

        const RenderContext ctx = BuildRenderContext(deltaTime);
        // 每帧必须先清屏（颜色 + 深度），与 IModel::Render 的契约一致。
        // 双缓冲交换后 back buffer 内容按 GL 规范是未定义的，深度缓冲也会
        // 残留上一帧的值；不清屏时下一帧所有片元因深度相等被 GL_LESS 剔除，
        // 结果是 draw calls / FPS / GL 错误全部正常，画面却全空。
        // ClearTransparent() 内部会复位 colorMask / depthMask（半透明材质会
        // 把 depthMask 留成 GL_FALSE，否则 glClear 清不掉深度）。
        ClearTransparent();
        // 先推进语言系统（台词/气泡/口型），把口型权重写进模型，
        // 再 Update 让本帧的表情/口型生效。
        TickDialogue(deltaTime);
        if (model_) {
            model_->Update(ctx);
            model_->Render(ctx);
        }

        // 设置界面是独立 Qt 窗口；每帧手动驱动 Qt 事件分发，
        // 与 GLFW 主循环共用同一条主线程（无需独立 Qt 线程）。
        settingsPanel_.PumpEvents();

        // 先消费本帧 GL 错误：下面的帧像素读回内部会清空错误队列，
        // 若放到读回之后再采集，真实错误会被吞掉。
        const unsigned int frameGlError = ConsumeGlErrors();

        // 帧像素证据：面板可见时每 200 ms 抓帧（顺带统计），面板不可见时
        // 每 kEvidenceProbeFrames 帧做一次轻量读回自检，结果也写日志。
        // 这样「draw calls 正常但帧缓冲全空」不打开面板也能被发现。
        const bool refreshPanel = debugPanel_.ShouldRefresh();
        // safeMode 下不做 glReadPixels：它是同步阻塞 GPU 的调用，渲染异常或
        // 第三方 overlay 注入时可能长时间挂住主线程，反而放大卡顿。
        if (!safeMode_ &&
            (refreshPanel ||
             stability::SafeDelta(frameCount_, lastEvidenceFrame_) >= kEvidenceProbeFrames)) {
            CollectFrameEvidence(refreshPanel);
        }

        // 诊断面板：只有在可见且到达刷新时刻（200 ms）时才采集快照。
        // IModel::DebugInfo() 会检查贴图文件是否存在，每帧做会有不必要的 IO。
        if (refreshPanel) {
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
            stats.glError = frameGlError;

            // 语言系统运行时数据（供「表情与说话」诊断页签）。
            if (bubbleEnabled_ && !currentPhrase_.empty()) {
                char dbuf[96];
                std::snprintf(dbuf, sizeof(dbuf), "「%s」 %.0f%%",
                              currentPhrase_.c_str(),
                              static_cast<double>(speech_.Progress()) * 100.0);
                stats.dialogueStatus = dbuf;
            } else if (!bubbleEnabled_) {
                stats.dialogueStatus = "台词气泡已关闭";
            } else {
                stats.dialogueStatus = "（沉默）";
            }
            stats.lipOpen = lipSync_.MouthOpen();
            stats.lipForm = lipSync_.MouthForm();

            // 帧缓冲像素证据：让「正常渲染」结论建立在真实像素上。
            if (hasFrameEvidence_) {
                stats.hasFrameEvidence = true;
                stats.frameCoveragePercent = lastFrameStats_.coveragePercent;
                stats.frameNonTransparent = lastFrameStats_.nonTransparent;
                stats.frameTotalPixels = lastFrameStats_.totalPixels;
                stats.frameEvidenceWidth = lastFrameStats_.width;
                stats.frameEvidenceHeight = lastFrameStats_.height;
            }
            if (!lastPreviewPath_.empty()) {
                stats.renderPreviewPath = lastPreviewPath_;
                char note[160];
                std::snprintf(note, sizeof(note),
                              "渲染帧 %d x %d（非透明像素 %.2f%%，每 200 ms 抓取）",
                              lastFrameStats_.width, lastFrameStats_.height,
                              lastFrameStats_.coveragePercent);
                stats.renderPreviewNote = note;
            } else if (!lastEvidenceError_.empty()) {
                stats.renderPreviewNote = "渲染帧抓取失败：" + lastEvidenceError_;
            }

            ModelDebugInfo info = model_ ? model_->DebugInfo() : ModelDebugInfo();
            info.sourceArchive = lastFromZip_ ? lastPackage_.sourceArchive : std::string();
            info.glError = frameGlError;
            // 稳定 ID + 最近一次加载的详细错误（供诊断面板对照日志）。
            info.modelId = model_ ? MakeModelId(info.path, info.type) : std::string();
            if (!lastLoadError_.ok()) {
                info.errorCategory = lastLoadError_.CategoryName();
                info.errorDetails = lastLoadError_.details;
                info.errorDetails.insert(info.errorDetails.begin(),
                                         lastLoadError_.summary);
            }
            if (frameGlError != 0 && model_) {
                info.renderingOk = false;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "GL 错误码 %u", frameGlError);
                info.renderStatus = buf;
            }
            // 模型后端的 renderingOk 只看「网格/GPU/GL 错误」，无法证明有像素。
            // 这里用读回结果做最终裁决：一个非透明像素都没有 = 没出图。
            if (hasFrameEvidence_ && model_ && lastFrameStats_.coveragePercent <= 0.0) {
                info.renderingOk = false;
                info.renderStatus = "帧缓冲为空：模型未出图（0% 覆盖率）";
                info.errorCategory = "渲染无输出";
                info.errorDetails = {
                    "帧缓冲读回 0 个非透明像素：draw calls、贴图、FPS 与 GL 错误"
                    "都可能正常，但屏幕上没有画面。",
                    "常见原因：渲染前未清屏（尤其深度缓冲 / glDepthMask 被半透明"
                    "材质置为 GL_FALSE）、视口为 0、窗口透明合成丢失 alpha。",
                    "排查：确认主循环每帧调用 ClearTransparent()，"
                    "并用 ppet_render_verify 对该模型做离屏渲染验证。",
                };
            }
            debugPanel_.Update(info, stats);
        }
        debugPanel_.PumpEvents();

        window_.SwapBuffers();

        // 软限帧：glfwSwapInterval(1) 在部分驱动 / overlay 注入下会失效
        // （实测 xvfb 下 200+ FPS）。高频刷新透明分层窗口会持续占用 DWM
        // 合成与 GPU，一旦渲染异常就可能拖慢整个桌面，故按配置压到 60 FPS。
        const int effectiveFps =
            stability::EffectiveMaxFps(configuredMaxFps_, safeMode_);
        const double budget = stability::FrameBudgetSeconds(effectiveFps);
        if (budget > 0.0) {
            const double spent =
                std::chrono::duration<double>(clock::now() - now).count();
            if (spent < budget) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(budget - spent));
            }
        }
    }
}

void App::Shutdown() {
    console_.Stop();
    cursorTracker_.Stop();
    debugPanel_.Shutdown();
    settingsPanel_.Shutdown();
    if (bubble_) {
        bubble_->Shutdown();
        delete bubble_;
        bubble_ = nullptr;
    }
    tray_.Destroy();
    window_.Destroy();
    model_.reset();
    running_ = false;
}

}  // namespace ppet
