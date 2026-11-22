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
#include <src/tools/app/ConsoleFormat.hpp>

namespace ppet {

// 纯格式化/解析逻辑已抽到 console_fmt（可独立单测，见 ConsoleFormat.hpp）。
// 这里保留 using 只为让下方分派代码维持原有简短调用名。
using namespace console_fmt;

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
        // 采集 App 侧状态快照；分节渲染细节交给可单测的 console_fmt 模块。
        // 原先这里内联了约 105 行格式化代码，属 App 里最重的一块纯逻辑。
        DebugViewContext ctx;
        ctx.fallbackPath = cfg.modelPath;
        ctx.resources = model_->RenderResources();
        ctx.motionNames = model_->MotionNames();
        ctx.expressionIds = model_->ExpressionPresetIds();
        ctx.hasLoadError = !lastLoadError_.ok();
        ctx.errorCategory = lastLoadError_.CategoryName();
        ctx.errorSummary = lastLoadError_.summary;
        ctx.errorDetails = lastLoadError_.details;
        return FormatDebugInfo(info, a.empty() ? "all" : a[0], ctx);
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
        if (a.empty()) {
            return "用法: action list | action <name|idx> do|stop | action stop | "
                   "action random | action touch [head|body|leg|left|right]";
        }
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
            const bool ok = model_->StopMotion();
            if (ok) EmitPluginEvent(plugin::PluginEventKind::MotionStopped);
            return ok ? "Success: 已停止动作" : "fail: 停止动作失败";
        }
        // 需求 2：随机待机动作。
        if (first == "random") {
            if (!model_) return "fail: 当前没有模型";
            return model_->PlayRandomIdle() ? "Success: 随机待机动作"
                                            : "fail: 没有可用待机动作";
        }
        // 需求 2：分区触摸（head/body/leg/left/right）。
        if (first == "touch") {
            if (!model_) return "fail: 当前没有模型";
            const std::string region = a.size() > 1 ? a[1] : std::string("body");
            const bool ok = model_->TriggerTouch(region);
            if (ok) EmitPluginEvent(plugin::PluginEventKind::Interaction,
                                    std::string(), {{"region", region}});
            return ok ? ("Success: 触摸 " + region) : ("fail: 触摸 " + region + " 失败");
        }
        const std::string option = a.size() > 1 ? a[1] : std::string("do");
        std::string lowerOption = option;
        std::transform(lowerOption.begin(), lowerOption.end(), lowerOption.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerOption == "stop") {
            const bool ok = model_->StopMotion();
            if (ok) EmitPluginEvent(plugin::PluginEventKind::MotionStopped);
            return ok ? "Success: 已停止动作" : "fail: 停止动作失败";
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
            // 需求 6：广播「动作播放」事件，插件可据此联动（换表情/播台词等）。
            EmitPluginEvent(plugin::PluginEventKind::MotionPlayed, motionId);
            // 索引命中时回显「[idx] 名字」便于核对；名字命中时原样回显。
            const std::string echo =
                (motionId == a[0]) ? a[0] : ("[" + a[0] + "] " + motionId);
            return "Success: 播放动作 " + echo;
        }
        return "fail: 没有找到动作 " + motionId + "（用 action list 查看）";
    }

    // ---------------------------------------------------------------- plugin
    if (v == "plugin") {
        if (a.empty()) return "用法: plugin list | plugin <enable|disable> <name>";
        const std::string sub = a[0];
        if (sub == "list") {
            const auto status = pluginManager_.Status();
            if (status.empty()) return "当前没有已注册的插件。";
            std::vector<std::vector<std::string>> rows;
            for (const auto& kv : status) {
                rows.push_back({kv.first, kv.second ? "启用" : "禁用"});
            }
            return "插件列表：\n" + console::FormatTable({"名称", "状态"}, rows);
        }
        if (sub == "enable" || sub == "disable") {
            if (a.size() < 2) return "用法: plugin <enable|disable> <name>";
            const bool want = (sub == "enable");
            if (pluginManager_.SetEnabled(a[1], want)) {
                return "Success: 插件 " + a[1] + (want ? " 已启用" : " 已禁用");
            }
            return "fail: 找不到插件 " + a[1] + "（用 plugin list 查看）";
        }
        return "用法: plugin list | plugin <enable|disable> <name>";
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
