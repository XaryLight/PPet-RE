// PPet - C++ 移植版
// 控制台指令执行器。
//
// 所有命令都在**渲染主线程**执行（由 App::ProcessInput -> Console::Poll 驱动），
// 因此可以安全访问 model_ / configStore_ / 气泡窗口 / 托盘菜单等状态。
// stdin 读取在后台线程，只负责把字符串放进队列，不碰任何 App 状态。
//
// 输出约定（对齐用户给出的交互示例）：
//   > list                          Idx Name Path 表格，当前项带 *
//   > select 0                      "To turn 0 model Xxx" + "Success" / "fail: ..."
//   > debug status|res|...          分节的模型分析内容
//   > re model|draw|...            重载模型 / 重建渲染资源
//   > action list|<name|idx> do|stop  列出 / 按名字或索引播放 / 停止动作
//   > speak <txt>                  气泡文字 + 口型
#include <src/tools/app/App.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelNaming.hpp>
#include <src/tools/model/ModelError.hpp>

namespace ppet {
namespace {

/// 从路径取「文件名（去后缀）」。
/// 这里**刻意不碰 std::filesystem**：Windows 的 path(char*) 构造会做
/// char -> wide 的 UTF-8 严格转换，模型路径一旦含非法字节就会抛
/// `filesystem error: in __char_to_wide: Illegal byte sequence`，
/// 把 list 这种纯展示命令整条打断。显示名只是字节串切分，不需要文件系统。
std::string ConsoleBaseName(const std::string& path) {
    if (path.empty()) return "(未加载)";
    const std::string name = model_naming::FileStem(path);
    return name.empty() ? path : name;
}

/// 第 i 个模型的显示名（优先用配置里用户改过的名字）。
std::string ModelNameAt(const AppConfig& cfg, std::size_t index) {
    return model_naming::NameAt(cfg.modelList, cfg.modelNames, index);
}

/// 按路径找模型下标，找不到返回 -1。
int FindModelIndex(const AppConfig& cfg, const std::string& path) {
    for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
        if (cfg.modelList[i] == path) return static_cast<int>(i);
    }
    return -1;
}

/// 当前模型的显示名（列表里找不到时退回文件名）。
std::string CurrentModelName(const AppConfig& cfg) {
    const int idx = FindModelIndex(cfg, cfg.modelPath);
    return idx >= 0 ? ModelNameAt(cfg, static_cast<std::size_t>(idx))
                    : ConsoleBaseName(cfg.modelPath);
}

/// 失败结果判定：只有失败/异常/用法错误才写日志，正常指令只回显到控制台。
bool IsConsoleFailure(const std::string& result) {
    if (result.rfind("fail:", 0) == 0 || result.rfind("用法:", 0) == 0 ||
        result.rfind("未知指令:", 0) == 0) {
        return true;
    }
    return result.find("\nfail:") != std::string::npos ||
           result.find("\n用法:") != std::string::npos;
}

const char* TypeName(ModelType type) {
    switch (type) {
        case ModelType::Pmx: return "PMX";
        case ModelType::Vrm: return "VRM";
        case ModelType::Live2D: return "Live2D";
        case ModelType::Unknown:
        default: return "Unknown";
    }
}

bool ParseIndex(const std::string& text, int& out) {
    if (text.empty()) return false;
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::string Join(const std::vector<std::string>& items, const std::string& sep) {
    std::ostringstream os;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) os << sep;
        os << items[i];
    }
    return os.str();
}

std::string Failure(const ModelLoadError& err) {
    return "fail: " + (err.summary.empty() ? err.ToString() : err.summary);
}

/// 分节标题。
std::string Section(const std::string& name) {
    return "[" + name + "]";
}

}  // namespace

void App::SetConsoleOptions(bool force, std::string script) {
    consoleForce_ = force;
    consoleScript_ = std::move(script);
}

void App::SetupConsole() {
    console_.SetHandler([this](const console::Command& cmd) -> std::string {
        std::string result;
        try {
            result = ExecuteConsoleCommand(cmd);
        } catch (const std::exception& e) {
            // 异常必须留痕：这是排查问题的唯一线索。
            result = std::string("fail: 指令执行异常：") + e.what();
            PPET_LOG_TAG_DEBUG("CONSOLE", "> %s\n%s", cmd.raw.c_str(), result.c_str());
            return result;
        }
        // 正常执行的指令只回显到控制台 stdout、不写日志（避免刷屏）。
        // 失败/用法错误才落结构化日志；用 Debug 级别只进文件，不与 stdout 重复，
        // 多行结果借日志的多行能力分行展示。
        if (IsConsoleFailure(result)) {
            PPET_LOG_TAG_DEBUG("CONSOLE", "> %s\n%s", cmd.raw.c_str(), result.c_str());
        }
        return result;
    });

    if (!consoleScript_.empty()) {
        console_.SetEcho(false);  // 脚本模式：不回显命令，只打印结果
        console_.SubmitScript(consoleScript_);
    }
    console_.Start(consoleForce_);
}

std::string App::ExecuteConsoleCommand(const console::Command& cmd) {
    const std::string& v = cmd.verb;
    const std::vector<std::string>& a = cmd.args;
    AppConfig& cfg = configStore_.config();

    // ---------------------------------------------------------------- help
    if (v == "help" || v == "?") {
        return console::HelpText();
    }

    // ---------------------------------------------------------------- quit
    if (v == "quit" || v == "exit") {
        running_ = false;
        return "Bye.";
    }

    // ---------------------------------------------------------------- list
    if (v == "list" || v == "ls") {
        std::vector<std::vector<std::string>> rows;
        for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
            const std::string& path = cfg.modelList[i];
            const bool current = (path == cfg.modelPath);
            rows.push_back({std::to_string(i),
                            std::string(current ? "* " : "  ") + ModelNameAt(cfg, i),
                            path});
        }
        if (rows.empty()) return "模型列表为空。用 add <path> [name] 添加模型。";
        return console::FormatTable({"Idx", "Name", "Path"}, rows);
    }

    // ---------------------------------------------------------------- add
    if (v == "add") {
        const std::string path = a.empty() ? cmd.tail : a[0];
        if (path.empty()) return "用法: add <path> [name]";
        ModelLoadError err;
        if (!LoadModel(path, err, false)) return Failure(err);

        // 命名：可显式指定（参数可含空格），否则默认取文件名（去后缀）；
        // 与已有名字重复时自动标注序号。全程纯字符串处理，不做编码转换。
        std::string desired;
        if (a.size() > 1) {
            desired = Join(std::vector<std::string>(a.begin() + 1, a.end()), " ");
        } else {
            desired = model_naming::FileStem(path);
        }
        if (desired.empty()) desired = "模型";
        cfg.modelNames = model_naming::ResolveNames(cfg.modelList, cfg.modelNames);
        const int idx = FindModelIndex(cfg, cfg.modelPath);
        std::string finalName = desired;
        if (idx >= 0) {
            std::vector<std::string> others;
            for (std::size_t i = 0; i < cfg.modelNames.size(); ++i) {
                if (static_cast<int>(i) != idx) others.push_back(cfg.modelNames[i]);
            }
            finalName = model_naming::UniqueName(desired, others);
            cfg.modelNames[static_cast<std::size_t>(idx)] = finalName;
        }
        SaveConfig();
        RebuildTrayMenu();
        return "Added: " + path + "\nName : " + finalName + "\nSuccess";
    }

    // ---------------------------------------------------------------- rename
    if (v == "rename" || v == "name") {
        int idx = -1;
        if (a.empty() || !ParseIndex(a[0], idx)) return "用法: rename <idx> <name>";
        if (idx < 0 || idx >= static_cast<int>(cfg.modelList.size())) {
            return "fail: 下标越界";
        }
        std::string desired;
        if (a.size() > 1) {
            desired = Join(std::vector<std::string>(a.begin() + 1, a.end()), " ");
        }
        if (desired.empty()) {
            // 不传名字 = 恢复默认（文件名去后缀）。
            desired = model_naming::FileStem(cfg.modelList[static_cast<std::size_t>(idx)]);
        }
        if (desired.empty()) desired = "模型 " + std::to_string(idx + 1);
        cfg.modelNames = model_naming::ResolveNames(cfg.modelList, cfg.modelNames);
        std::vector<std::string> others;
        for (std::size_t i = 0; i < cfg.modelNames.size(); ++i) {
            if (static_cast<int>(i) != idx) others.push_back(cfg.modelNames[i]);
        }
        const std::string finalName = model_naming::UniqueName(desired, others);
        cfg.modelNames[static_cast<std::size_t>(idx)] = finalName;
        SaveConfig();
        RebuildTrayMenu();
        return "Renamed: " + finalName + "\nSuccess";
    }

    // ---------------------------------------------------------------- select
    if (v == "select" || v == "sel") {
        int idx = -1;
        if (a.empty() || !ParseIndex(a[0], idx)) return "用法: select <idx>";
        if (idx < 0 || idx >= static_cast<int>(cfg.modelList.size())) {
            return "fail: 下标越界（当前共 " + std::to_string(cfg.modelList.size()) + " 个模型）";
        }
        const std::string path = cfg.modelList[static_cast<std::size_t>(idx)];
        std::ostringstream os;
        os << "To turn " << idx << " model " << ModelNameAt(cfg, static_cast<std::size_t>(idx))
           << "\n";
        ModelLoadError err;
        if (!LoadModel(path, err, false)) {
            os << Failure(err);
            return os.str();
        }
        SaveConfig();
        RebuildTrayMenu();
        os << "Success";
        return os.str();
    }

    // ---------------------------------------------------------------- remove
    if (v == "remove" || v == "rm" || v == "delete") {
        int idx = -1;
        if (a.empty() || !ParseIndex(a[0], idx)) return "用法: remove <idx>";
        if (idx < 0 || idx >= static_cast<int>(cfg.modelList.size())) {
            return "fail: 下标越界";
        }
        const std::string removed = cfg.modelList[static_cast<std::size_t>(idx)];
        const bool wasCurrent = (removed == cfg.modelPath);
        cfg.modelList.erase(cfg.modelList.begin() + idx);
        // 名字列表必须与路径列表同步删除，否则后续项会整体错位。
        if (static_cast<std::size_t>(idx) < cfg.modelNames.size()) {
            cfg.modelNames.erase(cfg.modelNames.begin() + idx);
        }
        std::ostringstream os;
        os << "Removed: " << removed << "\n";
        if (wasCurrent && !cfg.modelList.empty()) {
            // 移除的是当前模型：切到新的第一项，避免显示一个不在列表里的模型。
            const std::string next = cfg.modelList.front();
            ModelLoadError err;
            if (!LoadModel(next, err, false)) {
                os << "切换替代模型失败：" << Failure(err);
                SaveConfig();
                RebuildTrayMenu();
                return os.str();
            }
            os << "Switched to: " << ConsoleBaseName(next) << "\n";
        } else if (wasCurrent) {
            cfg.modelPath.clear();
        }
        SaveConfig();
        RebuildTrayMenu();
        os << "Success";
        return os.str();
    }

    // ---------------------------------------------------------------- debug
    if (v == "debug" || v == "info") {
        if (!model_) return "fail: 当前没有模型";
        const ModelDebugInfo info = model_->DebugInfo();
        std::string section = a.empty() ? "all" : a[0];
        std::transform(section.begin(), section.end(), section.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool all = (section == "all" || section == "debuginfo");

        std::ostringstream os;
        os << "Model: " << info.displayName << " [" << TypeName(info.type) << "]\n";
        os << "Path : " << (info.path.empty() ? cfg.modelPath : info.path) << "\n";

        if (all || section == "status") {
            os << Section("status 渲染状态") << "\n"
               << "  loaded      : " << (info.loaded ? "true" : "false") << "\n"
               << "  gpuReady    : " << (info.gpuReady ? "true" : "false") << "\n"
               << "  renderingOk : " << (info.renderingOk ? "true" : "false") << "\n"
               << "  glError     : " << info.glError << "\n"
               << "  renderStatus: " << info.renderStatus << "\n"
               << "  modelId     : " << (info.modelId.empty() ? cfg.modelPath : info.modelId)
               << "\n";
        }
        if (all || section == "params" || section == "param") {
            os << Section("params 规模参数") << "\n"
               << "  vertices=" << info.vertexCount << "  indices=" << info.indexCount
               << "  triangles=" << info.triangleCount << "\n"
               << "  bones=" << info.boneCount << "  materials=" << info.materialCount
               << "  subMeshes=" << info.subMeshCount << "  morphs=" << info.morphCount << "\n"
               << "  textures=" << info.textureLoaded << "/" << info.textureReferenced
               << "  drawCalls=" << info.drawCalls << "\n"
               << "  modelHeight=" << info.modelHeight << "\n";
        }
        if (all || section == "res" || section == "resources") {
            os << Section("res 加载的资源") << "\n";
            const auto extra = model_->RenderResources();
            for (const auto& kv : extra) {
                os << "  " << kv.first << " : " << kv.second << "\n";
            }
            for (const ModelDebugInfo::TextureInfo& tex : info.textures) {
                os << "  texture : " << (tex.name.empty() ? "(无名)" : tex.name)
                   << "  " << tex.width << "x" << tex.height
                   << (tex.loaded ? "  [loaded]" : "  [missing]")
                   << (tex.path.empty() ? "" : "  " + tex.path) << "\n";
            }
            if (extra.empty() && info.textures.empty()) {
                os << "  (后端未提供资源清单)\n";
            }
            for (const std::string& missing : info.missingResources) {
                os << "  MISSING : " << missing << "\n";
            }
        }
        if (all || section == "bones") {
            os << Section("bones 骨架 (" + std::to_string(info.bones.size()) + ")") << "\n";
            const std::size_t limit = std::min<std::size_t>(info.bones.size(), 40);
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& b = info.bones[i];
                os << "  #" << b.index << " " << b.name << "  parent=" << b.parentIndex
                   << "  depth=" << b.depth << "  " << b.flags << "\n";
            }
            if (info.bones.size() > limit) {
                os << "  ... 其余 " << (info.bones.size() - limit) << " 根省略\n";
            }
        }
        if (all || section == "parts" || section == "part") {
            os << Section("parts 模型部分 (" + std::to_string(info.parts.size()) + ")") << "\n";
            const std::size_t limit = std::min<std::size_t>(info.parts.size(), 40);
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& p = info.parts[i];
                os << "  " << p.name << "  indices=" << p.indexCount
                   << "  material=" << p.materialIndex << "  " << p.detail << "\n";
            }
        }
        if (all || section == "motion" || section == "actions") {
            os << Section("motion 动作") << "\n"
               << "  status : " << info.motionStatus << "\n"
               << "  summary: " << info.motionSummary << "\n"
               << "  actions: " << Join(model_->MotionNames(), ", ") << "\n";
        }
        if (all || section == "expression" || section == "expr") {
            os << Section("expression 表情") << "\n"
               << "  status : " << info.expressionStatus << "\n"
               << "  summary: " << info.expressionSummary << "\n"
               << "  presets: " << Join(model_->ExpressionPresetIds(), ", ") << "\n";
        }
        if (all || section == "error" || section == "errors") {
            os << Section("error 错误与警告") << "\n";
            if (lastLoadError_.ok()) {
                os << "  (无)\n";
            } else {
                os << "  category: " << lastLoadError_.CategoryName() << "\n"
                   << "  summary : " << lastLoadError_.summary << "\n";
                for (const std::string& detail : lastLoadError_.details) {
                    os << "    - " << detail << "\n";
                }
            }
            os << "  dialogue: " << info.dialogueStatus << "\n";
        }
        if (all || section == "property" || section == "properties") {
            os << Section("property 性质") << "\n"
               << "  format=" << info.formatVersion << "  encoding=" << info.textEncoding
               << "  skinned=" << info.isSkinned << "  outline=" << info.hasOutline << "\n";
            for (const std::string& p : info.properties) os << "  " << p << "\n";
        }
        return os.str();
    }

    // ---------------------------------------------------------------- re
    if (v == "re" || v == "reload") {
        std::string what = a.empty() ? "all" : a[0];
        std::transform(what.begin(), what.end(), what.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool doModel = (what == "model" || what == "all");
        const bool doDraw = (what == "draw" || what == "all" || what == "render");
        if (!doModel && !doDraw) return "用法: re [model|draw|all]";

        std::ostringstream os;
        if (doModel) {
            ModelLoadError err;
            if (!LoadModel(cfg.modelPath, err, false)) {
                os << "re model -> " << Failure(err) << "\n";
            } else {
                os << "re model -> Success (" << CurrentModelName(cfg) << ")\n";
                RebuildTrayMenu();
            }
        }
        if (doDraw) {
            if (!model_) {
                os << "re draw -> fail: 当前没有模型\n";
            } else {
                model_->ReloadRenderer();
                os << "re draw -> Success（下一帧重建 GPU 资源）\n";
            }
        }
        return os.str();
    }

    // ---------------------------------------------------------------- action
    if (v == "action" || v == "motion") {
        if (!model_) return "fail: 当前没有模型";
        if (a.empty()) return "用法: action list | action <name> do|stop | action stop";
        std::string first = a[0];
        std::transform(first.begin(), first.end(), first.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (first == "list" || first == "ls") {
            const std::vector<std::string> names = model_->MotionNames();
            if (names.empty()) return "当前模型没有可列出的动作。";
            std::vector<std::vector<std::string>> rows;
            for (std::size_t i = 0; i < names.size(); ++i) {
                rows.push_back({std::to_string(i), names[i]});
            }
            return console::FormatTable({"Idx", "Action"}, rows);
        }
        if (first == "stop") {
            return model_->StopMotion() ? "Success: 已停止动作" : "fail: 停止动作失败";
        }
        const std::string option = a.size() > 1 ? a[1] : std::string("do");
        std::string lowerOption = option;
        std::transform(lowerOption.begin(), lowerOption.end(), lowerOption.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerOption == "stop") {
            return model_->StopMotion() ? "Success: 已停止动作" : "fail: 停止动作失败";
        }
        if (lowerOption != "do" && lowerOption != "play" && lowerOption != "start") {
            return "用法: action <name|idx> do|stop（未知选项 " + option + "）";
        }
        // 支持按索引调试动画：token 是纯数字且在范围内时映射成对应动作名，
        // 与 `action list` 展示的 Idx 列一致；否则按名字 / 模糊匹配。
        const std::vector<std::string> motionNames = model_->MotionNames();
        const auto target = console::ResolveMotionTarget(a[0], motionNames);
        if (!target.second) {
            return "fail: 动作索引 " + a[0] + " 越界（可用范围 0.." +
                   std::to_string(motionNames.empty() ? 0 : motionNames.size() - 1) +
                   "，用 action list 查看）";
        }
        const std::string& motionId = target.first;
        if (model_->PlayMotion(motionId)) {
            // 索引命中时回显「[idx] 名字」便于核对；名字命中时原样回显。
            const std::string echo =
                (motionId == a[0]) ? a[0] : ("[" + a[0] + "] " + motionId);
            return "Success: 播放动作 " + echo;
        }
        return "fail: 没有找到动作 " + motionId + "（用 action list 查看）";
    }

    // ---------------------------------------------------------------- expression
    if (v == "expression" || v == "expr" || v == "face") {
        if (!model_) return "fail: 当前没有模型";
        if (a.empty()) return "用法: expression list | expression <id>";
        std::string first = a[0];
        std::transform(first.begin(), first.end(), first.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (first == "list" || first == "ls") {
            const std::vector<std::string> ids = model_->ExpressionPresetIds();
            if (ids.empty()) return "当前模型没有表情预设。";
            std::vector<std::vector<std::string>> rows;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                rows.push_back({std::to_string(i), ids[i]});
            }
            return console::FormatTable({"Idx", "Expression"}, rows);
        }
        return model_->SetExpressionPreset(a[0])
                   ? "Success: 表情 -> " + a[0]
                   : "fail: 没有找到表情 " + a[0] + "（用 expression list 查看）";
    }

    // ---------------------------------------------------------------- speak
    if (v == "speak" || v == "say") {
        const std::string text = cmd.tail.empty() ? Join(a, " ") : cmd.tail;
        if (text.empty()) return "用法: speak <txt>";
        bubbleEnabled_ = true;
        lastSpeechKind_ = agent::AgentStateKind::Greeting;
        if (!speech_.Say(text, 2)) {
            return "fail: 台词系统繁忙（上一条还没说完）";
        }
        return "Speaking: " + text;
    }

    // ---------------------------------------------------------------- status
    if (v == "status" || v == "stat") {
        // 操作系统级光标查询：与「是否开启点击穿透 / 全屏追踪」无关，用于确认
        // 鼠标不在桌宠窗口上时仍拿得到全局屏幕坐标。
        Vec2 osCursor;
        const bool hasOsCursor = GlobalCursorTracker::QueryCursor(osCursor);
        const Vec2 winSize = window_.Size();
        std::ostringstream os;
        os << Section("status 运行时") << "\n"
           << "  frameCount : " << frameCount_ << "\n"
           << "  frameMs    : " << frameMsSmoothed_ << "\n"
           << "  fps        : "
           << (frameMsSmoothed_ > 0.001f ? 1000.0f / frameMsSmoothed_ : 0.0f) << "\n"
           << "  model      : " << CurrentModelName(cfg) << " ["
           << (model_ ? TypeName(model_->Type()) : "none") << "]\n"
           << "  speaking   : " << (speech_.IsSpeaking() ? "true" : "false") << "\n"
           << "  phrase     : " << currentPhrase_ << "\n"
           << "  lipOpen    : " << lipSync_.MouthOpen() << "\n"
           << "  bubble     : " << (bubbleEnabled_ ? "on" : "off") << "\n"
           // 用「是否以交互模式启动」而不是「stdin 读线程是否还活着」判断：
           // 管道喂入时 EOF 很快到达，线程已退出，但用户确实开的是交互控制台。
           << "  console    : "
           << (console_.Interactive() ? "interactive"
                                      : (!consoleScript_.empty() ? "script" : "off"))
           << "\n"
           // 长期运行可观测性：估算运行时长、被队列上限丢弃的命令数。
           << "  uptime     : "
           << static_cast<double>(frameCount_) * frameMsSmoothed_ / 1000.0 << " s\n"
           << "  cmdDropped : " << console_.DroppedLines() << "\n"
           << "  autoFitWin : " << (cfg.autoFitWindow ? "on" : "off") << "\n"
           << "  safeMode   : " << (safeMode_ ? "on" : "off") << "\n"
           << "  maxFps     : " << configuredMaxFps_
           << "（0=不限帧，仅依赖 VSync）\n"
           << "  armRestDrop: " << cfg.armRestDropDeg << " deg (0=后端默认)\n"
           << "  window     : " << static_cast<int>(winSize.x) << "x"
           << static_cast<int>(winSize.y) << "\n"
           << "  bubbleMaxW : " << (bubble_ ? bubble_->MaxWidth() : 0) << " px\n"
           << "  osCursor   : "
           << (hasOsCursor ? "(" + std::to_string(static_cast<int>(osCursor.x)) + "," +
                                 std::to_string(static_cast<int>(osCursor.y)) + ")"
                           : "unavailable")
           << " (与点击穿透无关)\n";
        return os.str();
    }

    return "未知指令: " + v + "（输入 help 查看支持的指令）";
}

}  // namespace ppet
