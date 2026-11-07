// PPet - C++ 移植版
// 应用配置：持久化到 JSON 文件，替代原 Electron 版的 electron-store。
#pragma once

#include <string>
#include <vector>

namespace ppet {

enum class Language {
    Chinese,
    English,
};

/// 应用设置。
struct AppConfig {
    /// 窗口始终置顶。
    bool alwaysOnTop = true;

    /// 忽略鼠标点击（点击穿透到下层窗口）。
    bool ignoreMouseEvents = false;

    /// 是否显示工具栏。
    bool showTool = true;

    /// 界面语言。
    Language language = Language::Chinese;

    /// 当前模型路径（可为本地绝对路径或在线 URL）。
    std::string modelPath;

    /// 模型列表。
    std::vector<std::string> modelList;

    /// 渲染缩放（1.0 为原始大小）。
    float renderScale = 1.0f;

    /// 窗口位置与尺寸，用于恢复上次摆放。
    int windowX = 100;
    int windowY = 100;
    int windowWidth = 350;
    int windowHeight = 600;

    /// 是否开机启动。
    bool openAtLogin = false;

    /// GitHub 代理开关（用于下载在线模型）。
    bool useGhProxy = true;

    /// 【全屏追踪】是否启用全局鼠标追踪。
    /// 关闭后模型只在自身窗口范围内跟随鼠标。
    bool fullscreenTracking = true;

    /// 【全屏追踪】追踪半径（像素）：全局鼠标距模型中心该距离时，
    /// 模型朝向达到最大偏转。值越大，追踪越「迟钝」。
    float trackingRadius = 400.0f;

    /// 【全屏追踪】最大偏转角度（度）。
    float maxTrackingAngle = 30.0f;
};

/// 配置存储：读写 JSON 文件，字段缺失时使用默认值（向后兼容）。
class ConfigStore {
public:
    ConfigStore() = default;

    /// 加载配置。文件不存在时返回默认配置且不报错。
    bool Load(const std::string& path, std::string& error);

    /// 保存配置。失败时 error 写入原因。
    bool Save(const std::string& path, std::string& error) const;

    /// 取得默认配置文件路径（按平台惯例）。
    static std::string DefaultConfigPath();

    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }

private:
    AppConfig config_;
};

}  // namespace ppet
