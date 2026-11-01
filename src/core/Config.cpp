// PPet - C++ 移植版
// 应用配置实现：JSON 持久化。
#include "core/Config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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

}  // namespace

std::string ConfigStore::DefaultConfigPath() {
    namespace fs = std::filesystem;

#if defined(_WIN32)
    // Windows 配置目录优先使用 %APPDATA%。
    if (const char* appData = std::getenv("APPDATA")) {
        return (fs::path(appData) / "PPet" / "config.json").string();
    }
#else
    if (const char* home = std::getenv("HOME")) {
#if defined(__APPLE__)
        return (fs::path(home) / "Library" / "Application Support" / "PPet" / "config.json").string();
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) return (fs::path(xdg) / "PPet" / "config.json").string();
        return (fs::path(home) / ".config" / "PPet" / "config.json").string();
#endif
    }
#endif

    // 兜底：使用当前工作目录。
    return "ppet-config.json";
}

bool ConfigStore::Load(const std::string& path, std::string& error) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // 首次运行，使用默认值即可，不视为错误。
        return true;
    }

    std::ifstream in(path);
    if (!in) {
        error = "无法打开配置文件：" + path;
        return false;
    }

    json j;
    try {
        in >> j;
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

    get_bool("alwaysOnTop", config_.alwaysOnTop);
    get_bool("ignoreMouseEvents", config_.ignoreMouseEvents);
    get_bool("showTool", config_.showTool);
    get_bool("openAtLogin", config_.openAtLogin);
    get_bool("useGhProxy", config_.useGhProxy);
    get_bool("fullscreenTracking", config_.fullscreenTracking);
    get_int("windowX", config_.windowX);
    get_int("windowY", config_.windowY);
    get_int("windowWidth", config_.windowWidth);
    get_int("windowHeight", config_.windowHeight);
    get_float("renderScale", config_.renderScale);
    get_float("trackingRadius", config_.trackingRadius);
    get_float("maxTrackingAngle", config_.maxTrackingAngle);

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

    // 合法性校验：越界的数值回退到默认范围，避免异常配置导致渲染错乱。
    if (config_.renderScale < 0.25f || config_.renderScale > 4.0f) config_.renderScale = 1.0f;
    if (config_.trackingRadius < 50.0f) config_.trackingRadius = 50.0f;
    if (config_.trackingRadius > 5000.0f) config_.trackingRadius = 5000.0f;
    if (config_.maxTrackingAngle < 0.0f || config_.maxTrackingAngle > 90.0f) {
        config_.maxTrackingAngle = 30.0f;
    }
    if (config_.windowWidth < 100 || config_.windowWidth > 4000) config_.windowWidth = 350;
    if (config_.windowHeight < 100 || config_.windowHeight > 4000) config_.windowHeight = 600;

    return true;
}

bool ConfigStore::Save(const std::string& path, std::string& error) const {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            error = "无法创建配置目录：" + p.parent_path().string() + "（" + ec.message() + "）";
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
    j["renderScale"] = config_.renderScale;
    j["windowX"] = config_.windowX;
    j["windowY"] = config_.windowY;
    j["windowWidth"] = config_.windowWidth;
    j["windowHeight"] = config_.windowHeight;
    j["trackingRadius"] = config_.trackingRadius;
    j["maxTrackingAngle"] = config_.maxTrackingAngle;

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "无法写入配置文件：" + path;
        return false;
    }
    out << j.dump(4);
    out.close();
    if (!out) {
        error = "配置文件写入过程中出错：" + path;
        return false;
    }
    return true;
}

}  // namespace ppet
