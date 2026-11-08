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

    /// 模型显示名（与 modelList 一一对应）。
    /// 空项表示「用文件名（去后缀）」；重名由 ModelNaming 自动加序号。
    /// 旧配置没有这个字段时 Load() 会按路径补齐，向后兼容。
    std::vector<std::string> modelNames;

    /// 渲染缩放（1.0 为原始大小）。
    float renderScale = 1.0f;

    /// 加载模型后按「模型实际帧宽高比」自动调整渲染窗口尺寸。
    /// 横向模型（宽 > 高）在固定的竖窗口里会被左右截断，开启后窗口自动变宽。
    bool autoFitWindow = true;

    /// 手臂自然下垂修正（度）。PMX 的 A-pose / VRM 的 T-pose 默认把手臂平举着，
    /// 桌宠看起来「总抬着手」。0 = 由后端按模型类型自动选择（PMX 18° / VRM 55°），
    /// 正数 = 强制使用指定角度，负数 = 关闭修正。
    float armRestDropDeg = 0.0f;

    /// 通过 GUI 导入模型后，是否弹出输入框让用户指定显示名。
    /// 不填/取消则使用默认名（文件名去后缀，重名自动加序号）。
    /// 脚本模式（--command）下自动跳过，避免阻塞无人值守运行。
    bool askModelNameOnImport = true;

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
