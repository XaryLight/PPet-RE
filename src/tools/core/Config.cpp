// PPet - C++ 移植版
// 应用配置实现：JSON 持久化。
#include <src/tools/core/Config.hpp>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include <src/tools/core/FileIO.hpp>
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

/// 配置目录候选顺序：优先系统约定位置，逐级回退到便携 / 当前目录。
///
/// 为什么需要回退：曾经出现 `%APPDATA%` 被指向 `A:\Roaming`（软驱 / 重定向卷 /
/// 不可写盘）的机器，ConfigStore 直接拿它拼出 `A:\Roaming\PPet\config.json`，
/// 每次保存都因 INVALID_HANDLE_VALUE 失败，却只报一句含糊的「无法写入配置文件」。
/// 因此这里只「列候选」，是否可用由 DirIsWritable 实测决定。
std::vector<std::string> CandidateConfigDirs() {
    std::vector<std::string> dirs;

#if defined(_WIN32)
    const std::string appData = GetEnvVar("APPDATA");
    if (!appData.empty()) dirs.push_back(JoinPath(appData, "PPet"));
    const std::string localAppData = GetEnvVar("LOCALAPPDATA");
    if (!localAppData.empty()) dirs.push_back(JoinPath(localAppData, "PPet"));
#else
    const std::string xdg = GetEnvVar("XDG_CONFIG_HOME");
    if (!xdg.empty()) dirs.push_back(JoinPath(xdg, "PPet"));
    const std::string home = GetEnvVar("HOME");
#if defined(__APPLE__)
    if (!home.empty()) {
        dirs.push_back(JoinPath(JoinPath(home, "Library/Application Support"), "PPet"));
    }
#else
    if (!home.empty()) dirs.push_back(JoinPath(JoinPath(home, ".config"), "PPet"));
#endif
#endif

    // 便携 / 兜底：可执行文件所在目录、当前工作目录。
    const std::string exeDir = ExecutableDir();
    if (!exeDir.empty()) dirs.push_back(JoinPath(exeDir, "PPet"));
    dirs.push_back("PPet");  // 相对当前工作目录

    // 去重，避免同一目录被重复实测。
    std::vector<std::string> unique;
    for (auto& d : dirs) {
        bool seen = false;
        for (auto& u : unique) {
            if (u == d) {
                seen = true;
                break;
            }
        }
        if (!seen) unique.push_back(std::move(d));
    }
    return unique;
}

/// 实测目录是否可写：能建目录 + 能落一个临时文件 + 能删掉。
/// 任何一个环节失败（权限不足 / 介质只读 / 路径非法）都返回 false。
bool DirIsWritable(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(ToFsPath(dir), ec);
    if (ec) return false;
    const std::string probe = JoinPath(dir, ".ppet_writetest");
    if (!WriteFileText(probe, "ok")) return false;
    fs::remove(ToFsPath(probe), ec);
    return true;
}

}  // namespace

std::string ConfigStore::DefaultConfigPath() {
    for (const auto& dir : CandidateConfigDirs()) {
        if (DirIsWritable(dir)) return JoinPath(dir, "config.json");
    }
    // 极端兜底：任何目录都写不了时退回当前目录散落文件，至少让程序能跑
    // （用户可手动把配置拷到可写位置）。
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
    const auto get_bool = [&](const char* key, bool& out) {
        if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    const auto get_int = [&](const char* key, int& out) {
        if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
    };
    const auto get_float = [&](const char* key, float& out) {
        if (j.contains(key) && j[key].is_number()) out = j[key].get<float>();
    };
    const auto get_str = [&](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
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

    std::error_code ec;
    const fs::path p = ToFsPath(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            error = "无法创建配置目录：" + FromFsPath(p.parent_path()) + "（" + ec.message() + "）";
            return false;
        }
    }

    json j;
    j["alwaysOnTop"] = config_.alwaysOnTop;
    j["ignoreMouseEvents"] = config_.ignoreMouseEvents;
    j["showTool"] = config_.showTool;
    j["openAtLogin"] = config_.openAtLogin;
    j["useGhProxy"] = config_.useGhProxy;
    j["fullscreenTracking"] = config_.fullscreenTracking;
    j["language"] = LanguageToString(config_.language);
    j["modelPath"] = config_.modelPath;
    j["modelList"] = config_.modelList;
    j["modelNames"] = model_naming::ResolveNames(config_.modelList, config_.modelNames);
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

    // 走跨平台写入接口：Windows 上窄字符路径无法写入含中文的目录。
    std::string sysErr;
    if (!WriteFileText(path, j.dump(4), &sysErr)) {
        error = "无法写入配置文件：" + path;
        if (!sysErr.empty()) error += "（" + sysErr + "）";
        return false;
    }
    return true;
}

}  // namespace ppet
