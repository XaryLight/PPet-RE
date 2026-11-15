// PPet - C++ 移植版
// 渲染窗口尺寸 / 渲染倍率：把「窗口该多大」做成纯逻辑，便于单测。
//
// 【Q2 背景】
//   旧实现把 renderScale 直接乘进模型的 contain fit：
//       k = min(vw/W, vh/H) * margin * renderScale
//   于是「模型占窗口比例」恒等于 margin * renderScale —— 与窗口尺寸**无关**。
//   只要倍率 > 1/margin（约 1.05），模型必然超出窗口客户区被截断；单纯把窗口
//   调大也救不了（占比不变）。用户看到的状态是 `window : 437x600` 固定不变，
//   而 1.0x -> 2.0x 后渲染图被切掉一半，即「渲染不完全」。
//
// 【正确语义】renderScale = 整个渲染窗口的缩放倍率（相对 1.0x 基准）。
//   - 倍率变化时按倍率等比缩放**窗口**；
//   - 模型 fit 恒按 1.0 倍 contain fit，占窗口约 95%，任何倍率下都不截断；
//   - 整体在屏幕上变大 renderScale 倍，状态栏 `window` 同步变化。
//
// 【纯逻辑约束】不 include GL / GLFW / Qt，可脱离窗口系统单测。
#pragma once

#include <algorithm>
#include <cmath>

namespace ppet {

struct WindowSize {
    int width = 0;
    int height = 0;
};

/// 按倍率等比缩放窗口，并在桌面可用范围内**整体**收缩以保持宽高比。
///
/// 不能对宽、高分别 clamp：桌面高度常常比宽度先到极限，分别钳制会把
/// 竖向模型压扁，重新引入「窗口与渲染图宽高比不匹配」的问题。
inline WindowSize ScaleWindowSize(int width, int height, float ratio, int minW,
                                  int minH, int maxW, int maxH) {
    if (!(ratio > 0.0f) || width <= 0 || height <= 0 ||
        maxW < minW || maxH < minH) {
        return {width, height};
    }
    float w = static_cast<float>(width) * ratio;
    float h = static_cast<float>(height) * ratio;
    const float shrink = std::min(
        1.0f, std::min(static_cast<float>(maxW) / std::max(w, 1.0f),
                       static_cast<float>(maxH) / std::max(h, 1.0f)));
    w *= shrink;
    h *= shrink;
    return {std::clamp(static_cast<int>(w + 0.5f), minW, maxW),
            std::clamp(static_cast<int>(h + 0.5f), minH, maxH)};
}

/// 由「调整前后的真实尺寸」反推实际生效的缩放比。
/// 被桌面尺寸限制时，实际倍率会小于目标倍率；据此更新基准可避免
/// 用户反复切换倍率时窗口尺寸逐步漂移。
inline float EffectiveScaleRatio(int beforeW, int beforeH, int afterW, int afterH) {
    if (beforeW <= 0 || beforeH <= 0) return 1.0f;
    return std::min(static_cast<float>(afterW) / static_cast<float>(beforeW),
                    static_cast<float>(afterH) / static_cast<float>(beforeH));
}

}  // namespace ppet
