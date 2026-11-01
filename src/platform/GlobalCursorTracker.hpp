// PPet - C++ 移植版
// 全局鼠标追踪：实现「全屏追踪」能力。
//
// 背景：原 Electron 版本只能在模型窗口内获得鼠标位置，模型眼睛只在小范围内
// 跟随鼠标（见仓库 issue #26 / #122 / #158）。本模块通过操作系统级 API 获取
// 整个虚拟桌面范围内的光标位置，使追踪范围不再受窗口边界限制。
//
// 平台实现：
//   - Windows: SetWindowsHookEx(WH_MOUSE_LL) 全局低级鼠标钩子
//   - macOS  : NSEvent 的 global monitor（需 AppKit）
//   - Linux  : X11 XRecord 扩展；若不可用则退化为轮询 XQueryPointer
#pragma once

#include <functional>
#include <string>

#include "core/Math.hpp"

namespace ppet {

/// 屏幕坐标系约定：
///   - 原点位于虚拟桌面左上角
///   - X 向右递增，Y 向下递增（与操作系统一致）
///   - 多显示器时坐标可超出单屏范围（负值表示左侧/上方显示器）
struct GlobalCursorState {
    Vec2 position{0.0f, 0.0f};  ///< 虚拟桌面坐标（像素）
    bool valid = false;         ///< 是否已获取到有效位置
    bool moved = false;          ///< 本帧是否发生移动
};

/// 虚拟桌面（所有显示器）的几何信息。
struct VirtualDesktop {
    int x = 0;           ///< 左边界（可为负）
    int y = 0;           ///< 上边界（可为负）
    int width = 1920;    ///< 总宽度
    int height = 1080;   ///< 总高度
};

/// 全局光标追踪器。
///
/// 使用方式：每帧调用 Update()，然后通过 Position() 读取最新位置。
/// 钩子回调会被投递到主线程处理，避免跨线程访问 UI 状态。
class GlobalCursorTracker {
public:
    GlobalCursorTracker() = default;
    ~GlobalCursorTracker();

    GlobalCursorTracker(const GlobalCursorTracker&) = delete;
    GlobalCursorTracker& operator=(const GlobalCursorTracker&) = delete;

    /// 启动追踪。失败时返回 false，error 中写入原因。
    bool Start(std::string& error);

    /// 停止追踪并释放系统资源。
    void Stop();

    bool IsRunning() const { return running_; }

    /// 每帧调用：拉取并归一化最新光标位置。
    void Update();

    /// 当前光标位置（虚拟桌面坐标）。
    Vec2 Position() const { return state_.position; }

    /// 本帧光标是否移动过。
    bool Moved() const { return state_.moved; }

    /// 位置是否有效。
    bool HasPosition() const { return state_.valid; }

    /// 虚拟桌面范围。
    VirtualDesktop Desktop() const { return desktop_; }

    /// 刷新虚拟桌面范围（显示器插拔、分辨率变化后需调用）。
    void RefreshDesktop();

    /// 内部使用：写入桌面范围。
    void SetDesktop(const VirtualDesktop& d) { desktop_ = d; }

    /// 将全局光标位置换算为「相对某个屏幕矩形中心」的归一化坐标。
    ///
    /// 这是全屏追踪的核心换算：不论鼠标位于屏幕何处，都能得到 [-1, 1] 区间的
    /// 方向向量，用于驱动模型眼球/头部朝向。
    ///
    /// @param cursor      全局光标位置
    /// @param center      参照中心（通常是模型在屏幕上的中心）
    /// @param radius      参照半径（像素）：光标距中心该距离时归一化坐标约为 1
    /// @param outNormalized 输出归一化坐标（x 右正，y 上正）
    static Vec2 NormalizeAround(const Vec2& cursor, const Vec2& center, float radius,
                                 Vec2* outNormalized);

private:
    // 各平台内部实现通过 pimpl 隔离，避免在头文件暴露平台头文件。
    // 注意：Impl 必须在 public 区域声明，否则 .cpp 中的平台钩子自由函数
    // （如 Windows 的 MouseHookProc）无法引用该类型。
public:
    struct Impl;
private:
    Impl* impl_ = nullptr;

    GlobalCursorState state_;
    VirtualDesktop desktop_;
    bool running_ = false;

    // 上一帧位置，用于计算 moved 标志。
    Vec2 lastPosition_{0.0f, 0.0f};
    bool hasLast_ = false;
};

/// 各平台填充虚拟桌面范围的实现（定义在 .cpp 中）。
void UpdateDesktopImpl(GlobalCursorTracker& tracker);

}  // namespace ppet
