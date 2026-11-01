// PPet - C++ 移植版
// 系统托盘层。
//
// 对应原 Electron 版的 electron.Tray（见原项目 src/main/tray.ts）。
//
// 职责划分：
//   - Tray 只负责「显示菜单 + 回调 id」，**不做 i18n**；
//     中文/英文标签由上层（UI 层）根据当前语言生成后通过 UpdateMenu 传入。
//   - 菜单项顺序与原项目保持一致：
//       @置顶 / 小工具 / 忽略点击 / 配置
//       ──────── 上一个模型 / 下一个模型 ────────
//       语言(子菜单) ──────── 重新渲染 / 调试 ────────
//       开机启动 ──────── 反馈 / 关于 ──────── 退出
//
// 平台实现：
//   - Windows: Shell_NotifyIconW + TrackPopupMenu（系统原生菜单）
//   - macOS  : NSStatusItem + NSMenu（系统原生菜单）
//   - Linux  : 纯 X11 实现 _NET_SYSTEM_TRAY(ayatana 协议) + XEmbed 停靠，
//              菜单为自绘弹出窗口；找不到托盘宿主时降级为「无托盘」模式。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ppet {

/// 托盘菜单项。
struct TrayMenuItem {
    enum class Type {
        Toggle,     ///< 开关项（勾选状态由上层维护）
        Action,     ///< 普通动作项
        Checkbox,   ///< 复选项
        Radio,      ///< 单选项
        Separator,  ///< 分隔线
        Submenu,    ///< 子菜单
    };

    Type type = Type::Action;
    std::string label;   ///< 显示文本（由上层做 i18n）
    std::string id;      ///< 回调标识，回调时原样传回
    bool checked = false;
    bool enabled = true;
    std::vector<TrayMenuItem> subItems;  ///< Submenu 用

    /// 快捷键提示（仅用于显示，不注册全局快捷键）。例如 "Ctrl+T"。
    /// Windows 上会显示在菜单右侧，macOS/Linux 上作为提示文本的一部分。
    std::string accelerator;
};

/// 系统托盘。
///
/// 典型用法：
/// @code
///   Tray tray;
///   std::string error;
///   if (!tray.Create(error)) { /* 降级：仅保留窗口内 UI */ }
///   tray.SetItemCallback([](const std::string& id, bool checked) { ... });
///   tray.UpdateMenu(BuildMenu());   // 勾选状态变化后重新调用
/// @endcode
class Tray {
public:
    /// 菜单项被选中时的回调。
    ///
    /// @param id      菜单项的 id（与 UpdateMenu 传入的一致）
    /// @param checked 勾选后的新状态：
    ///                - Toggle / Checkbox：切换后的状态
    ///                - Radio：true
    ///                - Action：该项原本的 checked 值（通常为 false）
    using ItemCallback = std::function<void(const std::string& id, bool checked)>;

    Tray() = default;
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    /// 创建托盘图标。
    /// 失败时返回 false，error 中写入中文原因。
    /// Linux 上找不到托盘宿主时会返回 false —— 这属于「无托盘降级模式」，
    /// 调用方应当继续运行（仅保留窗口内 UI），而不是退出程序。
    bool Create(std::string& error);

    /// 销毁托盘图标并释放平台资源。可重复调用。
    void Destroy();

    /// 设置鼠标悬停在图标上时的提示文本。
    void SetTooltip(const std::string& text);

    /// 设置托盘图标图片路径（PNG）。
    void SetIcon(const std::string& path);

    /// 重建菜单（勾选状态变化后调用）。
    void UpdateMenu(const std::vector<TrayMenuItem>& items);

    /// 设置菜单项回调。
    void SetItemCallback(ItemCallback cb);

    /// 弹出菜单。
    /// Windows/macOS 由系统在用户点击图标时触发；本接口用于「主动弹出」场景
    /// （例如右键托盘图标时 Windows 会直接弹菜单；macOS 保持为空实现）。
    void PopupMenu();

    // -------------------------------------------------------------------------
    // 扩展接口
    // -------------------------------------------------------------------------

    /// 托盘是否真实可用（false 表示处于「无托盘降级模式」）。
    bool IsAvailable() const { return available_; }

    /// 平台降级说明（中文），例如「未找到系统托盘宿主，已降级为无托盘模式」。
    const std::string& PlatformNote() const { return platformNote_; }

    /// 泵送托盘层产生的事件。
    ///
    /// Windows/macOS 上菜单由系统在主线程触发，此接口为空实现；
    /// Linux 上托盘运行在独立线程，需要由主线程每帧调用一次，
    /// 把「用户选中了菜单项」投递回主线程执行回调，避免跨线程访问 UI 状态。
    void PumpEvents();

    /// 平台层在用户选择菜单项时调用（index 为扁平化后的菜单项索引）。
    /// 属于内部接口，一般由上层通过 SetItemCallback 的回调间接使用。
    void NotifyItemSelected(int index);

private:
    /// 重建扁平索引表（items_ → flat_）。
    void RebuildFlatIndex();

    struct Impl;
    Impl* impl_ = nullptr;

    /// 完整菜单模型（副本，供平台层读取）。
    std::vector<TrayMenuItem> items_;
    /// 扁平索引表：flat_[i] 指向 items_ 中的某个菜单项（含子菜单项）。
    std::vector<const TrayMenuItem*> flat_;

    ItemCallback callback_;
    std::string platformNote_;
    bool available_ = false;
};

}  // namespace ppet
