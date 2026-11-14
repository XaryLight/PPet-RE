// PPet - C++ 移植版
// Live2D 真实内容帧：从「实际渲染出来的顶点」推算内容包围盒与动态像素比。
//
// 【为什么不能用画布尺寸】
//   Cubism 的 canvas.sizeInPixels 是**画布**（导出时的画板），通常远大于角色本身：
//     例：画布 2000x600，角色只占中间 600x600 的一块，四周大片透明留白。
//   旧实现 BuildTransform() 用画布宽高 W/H 做等比 fit：
//     k = min(vw/W, vh/H)
//   窗口 600x600 时 k = min(600/2000, 600/600) = 0.3，
//   角色被缩到 180px 宽 —— 又小又偏，而且一旦改成 cover 就会横向截断。
//   用户反馈的「真实像素 2000x600 / 框 600x600 / 横向被截断」正是这个问题。
//
// 【正确做法（官方 Framework 的 GetDrawableVertexPositions 语义）】
//   1. 遍历本帧所有**可见且有贴图**的 Drawable 顶点，求内容包围盒（模型单位）；
//   2. 用包围盒的宽高比作为「真实帧宽高比」，据此调整窗口（动态像素比）；
//   3. 渲染变换按包围盒**居中 + contain fit**，任何窗口比例下都完整不截断。
//
// 【纯逻辑约束】不 include GL / Cubism / Qt，可脱离 SDK 单测。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ppet::live2d {

/// 模型单位下的真实内容包围盒（顶点已由 Cubism Core 每帧重算）。
struct ContentFrame {
    float minX = 0.0f, maxX = 0.0f;
    float minY = 0.0f, maxY = 0.0f;
    bool valid = false;

    float Width() const { return valid ? maxX - minX : 0.0f; }
    float Height() const { return valid ? maxY - minY : 0.0f; }
    float CenterX() const { return valid ? (minX + maxX) * 0.5f : 0.0f; }
    float CenterY() const { return valid ? (minY + maxY) * 0.5f : 0.0f; }
    /// 真实内容宽高比（width / height）。无效时返回 0。
    float Aspect() const {
        const float h = Height();
        return (valid && h > 1e-6f && Width() > 1e-6f) ? Width() / h : 0.0f;
    }
};

/// 逐部件累加点云，求真实内容包围盒。O(顶点数)，无堆分配。
class ContentFrameBuilder {
public:
    /// 累加一个部件的顶点（x,y 交错）。include=false 的部件（不可见 / 无贴图的
    /// 纯遮罩几何）不计入，否则遮罩矩形会把包围盒撑大，重新引入截断。
    void AddDrawable(const float* positions, int vertexCount, bool include) {
        if (!include || !positions || vertexCount <= 0) return;
        for (int i = 0; i < vertexCount; ++i) {
            const float x = positions[static_cast<std::size_t>(i) * 2 + 0];
            const float y = positions[static_cast<std::size_t>(i) * 2 + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            if (!frame_.valid) {
                frame_.minX = frame_.maxX = x;
                frame_.minY = frame_.maxY = y;
                frame_.valid = true;
            } else {
                frame_.minX = std::min(frame_.minX, x);
                frame_.maxX = std::max(frame_.maxX, x);
                frame_.minY = std::min(frame_.minY, y);
                frame_.maxY = std::max(frame_.maxY, y);
            }
        }
    }

    const ContentFrame& Frame() const { return frame_; }

private:
    ContentFrame frame_;
};

/// 由真实内容宽高比推算「不截断」的窗口宽度（固定窗口高度）。
/// 这是需求 3 的「动态像素比」：横向模型自动得到更宽的窗口。
inline int FitWindowWidth(float contentAspect, int windowHeight, int minWidth = 64,
                          int maxWidth = 2048) {
    if (!(contentAspect > 0.05f) || !(contentAspect < 20.0f) || windowHeight <= 0) return 0;
    const float w = static_cast<float>(windowHeight) * contentAspect;
    return std::clamp(static_cast<int>(w + 0.5f), minWidth, maxWidth);
}

/// 由真实内容宽高比推算「不截断」的窗口高度（固定窗口宽度）。
inline int FitWindowHeight(float contentAspect, int windowWidth, int minHeight = 64,
                           int maxHeight = 2048) {
    if (!(contentAspect > 0.05f) || !(contentAspect < 20.0f) || windowWidth <= 0) return 0;
    const float h = static_cast<float>(windowWidth) / contentAspect;
    return std::clamp(static_cast<int>(h + 0.5f), minHeight, maxHeight);
}

/// 把内容包围盒完整居中放进视口，产出列主序 4x4 变换（可直接喂 glUniformMatrix4fv）。
///
/// contain fit：k = min(vw/W, vh/H)，取较小边铺满、留 fit 边距，
/// 因此**任何窗口比例下内容都不会被截断**（横向模型上下留白，纵向模型左右留白）。
/// 同时把内容中心平移到视口中心，修复「顶点不以画布原点为中心」导致的偏移。
///
/// @param out 列主序 float[16]
/// @return frame 或视口无效时返回 false（调用方回退到画布 fit）。
inline bool ComputeFitMatrix(const ContentFrame& frame, float viewportW, float viewportH,
                             float renderScale, float fit, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;

    const float W = frame.Width();
    const float H = frame.Height();
    if (!frame.valid || W <= 1e-6f || H <= 1e-6f || viewportW <= 0.0f ||
        viewportH <= 0.0f) {
        return false;
    }
    const float scale = renderScale > 0.0f ? renderScale : 1.0f;
    const float margin = (fit > 0.0f && fit <= 1.0f) ? fit : 0.95f;
    const float k = std::min(viewportW / W, viewportH / H) * margin * scale;
    const float sx = k / (viewportW * 0.5f);
    const float sy = k / (viewportH * 0.5f);

    // 列主序：m[col*4+row]。平移在第 3 列。
    out[0] = sx;
    out[5] = sy;
    out[12] = -frame.CenterX() * sx;
    out[13] = -frame.CenterY() * sy;
    return true;
}

/// 诊断：内容宽高比与画布宽高比的差异（用于日志/报告佐证「动态像素比」）。
inline float AspectDeviation(float contentAspect, float canvasAspect) {
    if (!(contentAspect > 0.0f) || !(canvasAspect > 0.0f)) return 0.0f;
    return std::fabs(contentAspect - canvasAspect) / canvasAspect;
}

}  // namespace ppet::live2d
