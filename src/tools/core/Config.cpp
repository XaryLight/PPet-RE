// PPet - C++ 移植版
// 应用配置实现：JSON 持久化。
#include <src/tools/core/Config.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#include <src/tools/core/FileIO.hpp>
#include <src/tools/core/IoError.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/core/ModelNaming.hpp>
#include "nlohmann/json.hpp"

namespace ppet {
namespace {

using nlohmann::json;

/// 语言与字符串的互转，便于 JSON 存储。
const char* LanguageToString(Language lang) {
    return lang == Language::Chinese ? "zh" : "en";
}

Language LanguageFromString(const std::string& s) {
    return s == "en" ? Language::English : Language::Chinese;
}

/// 配置落盘的「暂存」位置：可执行文件同目录下的 PPet/config.json。
///
/// 为什么需要它：在 `A:` 这类「资源管理器能写、但程序自己 CreateFileW 写不进
/// 去」的磁盘（软驱 / 重定向卷 / 瞬时未就绪）上，直写目标路径会失败。这时先把
/// 配置写到 exe 同目录（这里一定可写），再 std::filesystem::copy_file 到目标
/// 路径 —— CopyFileW 用的是另一套打开标志，往往能绕过直写失败的那类问题。
std::string StagingConfigPath() {
    const std::string exeDir = ExecutableDir();
    if (exeDir.empty()) return std::string();
    return FromFsPath(ToFsPath(exeDir) / "PPet" / "config.json");
}

/// 【2.3.7】把已写好的临时文件原子替换到目标路径。
/// POSIX：rename 同目录文件是原子的（覆盖目标）；Windows：std::filesystem::rename
/// 遇到已存在的目标会失败，故先删旧目标再 rename。即便如此，新内容在 rename 前
/// 已完整写入临时文件，写入中途崩溃只会留下完整的旧目标或新临时文件，绝不出现
/// 被截断的 config.json —— 这正是「原子保存」要防的核心风险。
bool AtomicReplace(const std::filesystem::path& stagedTmp,
                   const std::filesystem::path& target, std::error_code& ec) {
#if defined(_WIN32)
    std::filesystem::remove(target, ec);
    if (ec) return false;
#endif
    std::filesystem::rename(stagedTmp, target, ec);
    return !ec;
}

/// 把可能含非法 UTF-8 字节的字符串净化成合法 UTF-8（缺陷 2.1.5）。
/// ModelNaming 故意按纯字节处理名字以兼容非法编码路径，但 nlohmann::json::dump
/// 会对非法 UTF-8 抛 type_error.316。落盘前把无法解码的字节替换为 '?'，
/// 保证序列化永不因编码问题崩溃；显示名不符时最多是乱码，不影响功能。
std::string SanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        std::size_t extra = 0;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        if (extra == 0 || i + extra >= n) {
            out.push_back('?');
            ++i;
            continue;
        }
        bool ok = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(reinterpret_cast<const char*>(p + i), extra + 1);
        i += extra + 1;
    }
    return out;
}

/// 对字符串数组整体做 UTF-8 净化。
std::vector<std::string> SanitizeStrings(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto& s : in) out.push_back(SanitizeUtf8(s));
    return out;
}

}  // namespace

std::string ConfigStore::DefaultConfigPath() {
    // 配置的目标位置就是系统约定目录（用户机器上是 A:\Roaming\PPet\config.json）。
    // 这里只负责「算出一个合法路径」，能不能写、写不进怎么办，全部交给 Save 处理，
    // 不再因为某次实测不可写就把配置悄悄挪到别处。
#if defined(_WIN32)
    const std::string appData = GetEnvVar("APPDATA");
    if (!appData.empty()) {
        return FromFsPath(ToFsPath(appData) / "PPet" / "config.json");
    }
    const std::string localAppData = GetEnvVar("LOCALAPPDATA");
    if (!localAppData.empty()) {
        return FromFsPath(ToFsPath(localAppData) / "PPet" / "config.json");
    }
#else
    const std::string xdg = GetEnvVar("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        return FromFsPath(ToFsPath(xdg) / "PPet" / "config.json");
    }
    const std::string home = GetEnvVar("HOME");
    if (!home.empty()) {
#if defined(__APPLE__)
        return FromFsPath(ToFsPath(home) / "Library" / "Application Support" /
                          "PPet" / "config.json");
#else
        return FromFsPath(ToFsPath(home) / ".config" / "PPet" / "config.json");
#endif
    }
#endif

    // 兜底：环境变量缺失时退回 exe 同目录或当前目录，至少让程序能跑起来。
    const std::string exeDir = ExecutableDir();
    if (!exeDir.empty()) {
        return FromFsPath(ToFsPath(exeDir) / "PPet" / "config.json");
    }
    return "ppet-config.json";
}

bool ConfigStore::Load(const std::string& path, std::string& error) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(ToFsPath(path), ec)) {
        // 首次运行，使用默认值即可，不视为错误。
        return true;
    }

    // 走跨平台读取接口：Windows 上窄字符路径打不开含中文的路径。
    std::string text;
    std::string sysErr;
    if (!ReadFileText(path, text, &sysErr)) {
        error = "无法打开配置文件：" + path;
        if (!sysErr.empty()) error += "（" + sysErr + "）";
        return false;
    }

    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("配置文件解析失败：") + e.what();
        return false;
    }

    if (!j.is_object()) {
        error = "配置文件格式错误：根节点不是对象";
        return false;
    }

    // 逐字段读取，缺失的字段保持默认值 —— 保证旧版本配置文件仍可加载。
    // 所有 get 都在 try 保护下：超范围整数 get<int>() 会抛 out_of_range（缺陷 2.1.5），
    // 单个坏字段只跳过、用默认值，绝不让整个进程崩溃。
    const auto get_bool = [&](const char* key, bool& out) {
        try {
            if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
        } catch (const json::exception&) { /* 坏字段忽略，保留默认值 */ }
    };
    const auto get_int = [&](const char* key, int& out) {
        try {
            if (!j.contains(key) || !j[key].is_number_integer()) return;
            // 先取 int64 再比较范围，避免 get<int>() 对超范围值直接抛异常。
            const std::int64_t v = j[key].get<std::int64_t>();
            if (v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max()) {
                out = static_cast<int>(v);
            }
        } catch (const json::exception&) { /* 坏字段忽略 */ }
    };
    const auto get_float = [&](const char* key, float& out) {
        try {
            if (j.contains(key) && j[key].is_number()) {
                const double v = j[key].get<double>();
                out = static_cast<float>(v);
            }
        } catch (const json::exception&) { /* 坏字段忽略 */ }
    };
    const auto get_str = [&](const char* key, std::string& out) {
        try {
            if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
        } catch (const json::exception&) { /* 坏字段忽略 */ }
    };

    const auto get_string_array = [&](const char* key, std::vector<std::string>& out) {
        if (!j.contains(key) || !j[key].is_array()) return;
        out.clear();
        for (const auto& item : j[key]) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    };

    get_bool("alwaysOnTop", config_.alwaysOnTop);
    get_bool("ignoreMouseEvents", config_.ignoreMouseEvents);
    get_bool("showTool", config_.showTool);
    get_bool("openAtLogin", config_.openAtLogin);
    get_bool("useGhProxy", config_.useGhProxy);
    get_bool("fullscreenTracking", config_.fullscreenTracking);
    get_bool("autoFitWindow", config_.autoFitWindow);
    get_bool("askModelNameOnImport", config_.askModelNameOnImport);
    get_int("windowX", config_.windowX);
    get_int("windowY", config_.windowY);
    get_int("windowWidth", config_.windowWidth);
    get_int("windowHeight", config_.windowHeight);
    get_float("renderScale", config_.renderScale);
    get_float("trackingRadius", config_.trackingRadius);
    get_float("maxTrackingAngle", config_.maxTrackingAngle);
    get_float("armRestDropDeg", config_.armRestDropDeg);
    get_int("maxFps", config_.maxFps);
    get_bool("safeMode", config_.safeMode);

    if (j.contains("language") && j["language"].is_string()) {
        config_.language = LanguageFromString(j["language"].get<std::string>());
    }
    get_str("modelPath", config_.modelPath);

    if (j.contains("modelList") && j["modelList"].is_array()) {
        config_.modelList.clear();
        for (const auto& item : j["modelList"]) {
            if (item.is_string()) config_.modelList.push_back(item.get<std::string>());
        }
    }
    get_string_array("modelNames", config_.modelNames);
    // 显示名与路径列表对齐：旧配置没有名字时按「文件名去后缀」补齐，
    // 名字重复时自动标注序号（纯字符串处理，不做编码转换，不会抛 filesystem 异常）。
    config_.modelNames = model_naming::ResolveNames(config_.modelList, config_.modelNames);

    // 合法性校验：越界的数值回退到默认范围，避免异常配置导致渲染错乱。
    if (config_.renderScale < 0.25f || config_.renderScale > 4.0f) config_.renderScale = 1.0f;
    if (config_.trackingRadius < 50.0f) config_.trackingRadius = 50.0f;
    if (config_.trackingRadius > 5000.0f) config_.trackingRadius = 5000.0f;
    if (config_.maxTrackingAngle < 0.0f || config_.maxTrackingAngle > 90.0f) {
        config_.maxTrackingAngle = 30.0f;
    }
    if (config_.armRestDropDeg < -1.0f || config_.armRestDropDeg > 90.0f) {
        config_.armRestDropDeg = 0.0f;
    }
    if (config_.windowWidth < 100 || config_.windowWidth > 4000) config_.windowWidth = 350;
    if (config_.windowHeight < 100 || config_.windowHeight > 4000) config_.windowHeight = 600;
    // 0 表示不限帧（合法）；负值或超过 240 的配置回落到默认 60。
    if (config_.maxFps < 0 || config_.maxFps > 240) config_.maxFps = 60;

    return true;
}

bool ConfigStore::Save(const std::string& path, std::string& error) const {
    namespace fs = std::filesystem;

    // 先把要落盘的内容一次性拼好，两路尝试（直写 / 暂存后拷贝）复用同一份。
    json j;
    j["alwaysOnTop"] = config_.alwaysOnTop;
    j["ignoreMouseEvents"] = config_.ignoreMouseEvents;
    j["showTool"] = config_.showTool;
    j["openAtLogin"] = config_.openAtLogin;
    j["useGhProxy"] = config_.useGhProxy;
    j["fullscreenTracking"] = config_.fullscreenTracking;
    j["language"] = LanguageToString(config_.language);
    j["modelPath"] = SanitizeUtf8(config_.modelPath);
    // modelList/modelNames 含从文件系统读来的字节，可能不是合法 UTF-8；
    // 序列化前统一净化，避免 dump() 抛 type_error.316 导致整个进程崩溃。
    j["modelList"] = SanitizeStrings(config_.modelList);
    j["modelNames"] = SanitizeStrings(
        model_naming::ResolveNames(config_.modelList, config_.modelNames));
    j["renderScale"] = config_.renderScale;
    j["autoFitWindow"] = config_.autoFitWindow;
    j["armRestDropDeg"] = config_.armRestDropDeg;
    j["askModelNameOnImport"] = config_.askModelNameOnImport;
    j["windowX"] = config_.windowX;
    j["windowY"] = config_.windowY;
    j["windowWidth"] = config_.windowWidth;
    j["windowHeight"] = config_.windowHeight;
    j["trackingRadius"] = config_.trackingRadius;
    j["maxTrackingAngle"] = config_.maxTrackingAngle;
    j["maxFps"] = config_.maxFps;
    j["safeMode"] = config_.safeMode;

    // 最后一道保险：即使有未预料的字段含非法编码，dump 失败也只返回错误，
    // 不让异常穿透到调用方（缺陷 2.1.5）。
    std::string text;
    try {
        text = j.dump(4);
    } catch (const json::exception& e) {
        error = std::string("配置信息序列化失败：") + e.what();
        PPET_LOG_TAG_ERR("CFG", "%s", error.c_str());
        return false;
    }

    // 每一步失败都往 error 里**追加**一条带系统错误码与中文原因的描述，
    // 最后统一写 ERR 日志。这样「直写失败 + 暂存拷贝也失败」时两路原因都在，
    // 不会出现只剩一句「无法写入配置文件」的静默失败。
    auto appendError = [&](std::string_view operation, std::string_view target,
                           std::string_view reason) {
        if (!error.empty()) error += "；";
        error += DescribeIoFailure(operation, target, reason);
    };

    // 第 1 路：原子写 —— 先写目标同目录的临时文件，再原子替换到目标路径
    // （报告 2.3.7）。这样写入中途崩溃只会留下完整的旧配置或临时文件，
    // 不会像直写覆盖那样留下被截断的 config.json 导致下次 Load 永久失败。
    // 【保留 A: 盘流程】若本路任一步失败（含 A: 盘 rename 失败），清理临时文件后
    // fall through 到第 2 路「直写 + 暂存拷贝」回退链，行为与历史完全一致。
    {
        std::error_code ec;
        const fs::path p = ToFsPath(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
            if (ec) {
                appendError("无法创建配置目录", FromFsPath(p.parent_path()),
                            IoErrorText(ec));
                PPET_LOG_TAG_ERR("CFG", "%s", error.c_str());
                return false;
            }
        }
        const fs::path tmp = p.string() + ".tmp";
        std::string sysErr;
        if (WriteFileText(FromFsPath(tmp), text, &sysErr)) {
            std::error_code renameEc;
            if (AtomicReplace(tmp, p, renameEc)) return true;
            appendError("原子替换配置失败（将尝试回退写盘）", path, IoErrorText(renameEc));
        } else {
            appendError("无法写入配置临时文件（将尝试回退写盘）", FromFsPath(tmp),
                        IoErrorText(sysErr));
        }
        // 清理可能残留的临时文件（A: 盘上写入成功但替换失败时会残留）。
        std::error_code cleanEc;
        fs::remove(tmp, cleanEc);
    }

    // 第 2 路：直写失败（如 A: 这类「资源管理器能写、程序 CreateFileW 写不进」的盘），
    // 先把配置落到 exe 同目录的暂存文件，再用 copy_file 拷到目标路径。
    // CopyFileW 走的是另一套打开标志，通常能绕过直写失败的那类问题。
    const std::string staged = StagingConfigPath();
    if (!staged.empty()) {
        std::error_code ec;
        fs::create_directories(ToFsPath(staged).parent_path(), ec);
        if (!ec) {
            std::string sysErr;
            if (WriteFileText(staged, text, &sysErr)) {
                std::error_code mkEc;
                fs::create_directories(ToFsPath(path).parent_path(), mkEc);
                if (mkEc) {
                    // 独立检查：早期把 create_directories 与 copy_file 共用同一个
                    // error_code，copy_file 成功会把前面的错误清掉，掩盖真实原因。
                    appendError("无法创建配置目录（暂存拷贝阶段）",
                                FromFsPath(ToFsPath(path).parent_path()),
                                IoErrorText(mkEc));
                } else {
                    std::error_code cec;
                    fs::copy_file(ToFsPath(staged), ToFsPath(path),
                                  fs::copy_options::overwrite_existing, cec);
                    if (!cec) {
                        // 拷过去就成功了，清掉暂存文件。
                        std::error_code rec;
                        fs::remove(ToFsPath(staged), rec);
                        return true;
                    }
                    appendError("已生成暂存文件，但拷贝到目标失败", path,
                                IoErrorText(cec));
                }
            } else {
                appendError("无法在程序同目录生成暂存配置", staged,
                            IoErrorText(sysErr));
            }
        } else {
            appendError("无法创建暂存目录",
                        FromFsPath(ToFsPath(staged).parent_path()), IoErrorText(ec));
        }
    }

    PPET_LOG_TAG_ERR("CFG", "%s", error.c_str());
    return false;
}

}  // namespace ppet
