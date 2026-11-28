// PPet - C++ 移植版
// 窗口几何查询：桌面坐姿（缺陷 4）的「空间感知」基础设施。
//
// 桌宠「坐到窗口上边缘」需要知道用户正在使用的窗口在哪。本模块提供两个平台
// 无关的查询接口，返回虚拟桌面坐标系（左上原点、y 向下，与 GlobalCursorTracker
// 一致）下的窗口边框矩形。
//
// 平台实现：
//   - Windows: GetForegroundWindow + DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)
//   - macOS  : CGWindowListCopyWindowInfo（取最前用户窗口）
//   - Linux  : X11 _NET_ACTIVE_WINDOW + XTranslateCoordinates
//
// Wayland 纯会话下 X11 查询不可用（返回 invalid），坐姿行为据此自动禁用——与
// 仓库现有的 Wayland 处理策略（光标追踪退化）保持一致。
#pragma once

#include <src/tools/core/Math.hpp>

namespace ppet {

/// 一个窗口在虚拟桌面坐标系的边框矩形（左上原点、y 向下）。
struct WindowRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = false;  ///< 是否成功获取（false 表示不可用，坐姿自动禁用）

    float TopY() const { return y; }
    float BottomY() const { return y + height; }
    float LeftX() const { return x; }
    float RightX() const { return x + width; }
    float CenterX() const { return x + width * 0.5f; }

    /// 光标（屏幕坐标）是否落在矩形内。
    bool Contains(float cx, float cy) const {
        return valid && cx >= x && cx <= x + width && cy >= y && cy <= y + height;
    }

    /// 是否空矩形（未获取到 / 尺寸非法）。
    bool Empty() const { return !valid || width <= 0.0f || height <= 0.0f; }
};

/// 窗口几何查询（平台 API）。
///
/// 所有方法都是静态、无状态、可每帧调用；失败时返回 valid=false，调用方据此
/// 降级（关闭坐姿、回退到现有全局光标追踪）。不安装钩子、不缓存跨帧句柄。
class WindowGeometry {
public:
    /// 前台（活跃）窗口边框矩形。
    ///
    /// 桌宠窗口开启点击穿透时不抢输入焦点，因此「前台窗口」通常是用户正在
    /// 交互的应用窗口，天然排除了桌宠自身。Windows 的 GetForegroundWindow 与
    /// X11 的 _NET_ACTIVE_WINDOW 都反映「当前活跃窗口」。
    static WindowRect ForegroundWindow();

    /// 光标所在窗口的边框矩形（光标位置为虚拟桌面坐标）。
    ///
    /// 部分平台（X11 无 EWMH 合成器 / Wayland）可能无法准确判定光标下的顶层
    /// 窗口，此时返回 invalid；调用方应回退到 ForegroundWindow。
    static WindowRect WindowUnderCursor(float cursorX, float cursorY);
};

}  // namespace ppet
