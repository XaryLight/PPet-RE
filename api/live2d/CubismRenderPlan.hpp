// PPet - C++ 移植版
// Live2D 渲染计划：把「画哪些部件、按什么顺序、用什么混合」从 GL 代码里分离。
//
// 【参考设计：完全基于 C++ 渲染的 Live2D 项目】
//   live2dwallpaper / CubismNativeSamples / CubismNativeFramework 这类项目
//   不用 Chromium/Electron 包装，C++ 直接遍历 Cubism Core 的 Drawable 绘制。
//   官方 OpenGL 渲染器（Framework/src/Rendering/OpenGL/）每帧的决策链是：
//     1. csmResetDrawableDynamicFlags(model)
//     2. 写入参数 → csmUpdateModel(model)
//     3. GetModelColorWithOpacity：premultiplied 下 RGB 与 A 一起乘 opacity
//     4. 按 csmGetDrawableRenderOrders 排序（注意不是 DrawOrder）
//     5. 跳过不可见 / 透明度为 0 / 无贴图的纯几何遮罩
//        （注意：官方会照常绘制「兼作遮罩源的可见部件」，不能按 isMaskDrawable 剔除）
//     6. 逐个设置 glBlendFuncSeparate、裁剪遮罩、顶点缓冲 → glDrawElements
//   早期本项目把这些决策散落在 DrawDrawable() 的 GL 调用里，难以单测、也容易
//   漏掉某一步（例如「RGB 也要乘 opacity」曾导致整幅画面偏白）。
//   本模块把 3~6 步做成**纯函数**，只输出一张「渲染指令表」，由 GL 层执行。
//
// 【纯逻辑约束】不 include 任何 GL / Cubism / Qt 头文件，可脱离 SDK 单测。
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ppet::live2d {

/// GL 混合因子的抽象（GL 层负责映射到 GL_ONE / GL_ZERO / ...）。
/// 用枚举而不是 GL 常量，是为了让本模块保持零依赖、可单测。
enum class BlendFactor {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstColor,
};

/// Cubism 部件的三种混合模式（与官方 CubismRenderer 一致）。
enum class DrawableBlend {
    Normal,
    Additive,      ///< 加法：发光/高光
    Multiplicative ///< 乘法：阴影
};

/// 官方 glBlendFuncSeparate 的三套参数表。
/// 权威依据：Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.cpp
///   Normal：RGB = ONE / ONE_MINUS_SRC_ALPHA,        A = ONE / ONE_MINUS_SRC_ALPHA
///   Add   ：RGB = ONE / ONE,                       A = ZERO / ONE
///   Mult  ：RGB = DST_COLOR / ONE_MINUS_SRC_ALPHA,  A = ZERO / ONE
///
/// Alpha 通道对 Add/Mult 必须设为 ZERO/ONE：桌宠窗口是透明叠加窗口，
/// 发光特效只该叠加 RGB，不能累加不透明度，否则会变成一块不透明白底。
struct BlendFunc {
    BlendFactor srcRgb = BlendFactor::One;
    BlendFactor dstRgb = BlendFactor::OneMinusSrcAlpha;
    BlendFactor srcA = BlendFactor::One;
    BlendFactor dstA = BlendFactor::OneMinusSrcAlpha;
};

inline BlendFunc BlendFuncFor(DrawableBlend mode) {
    switch (mode) {
        case DrawableBlend::Additive:
            return {BlendFactor::One, BlendFactor::One, BlendFactor::Zero, BlendFactor::One};
        case DrawableBlend::Multiplicative:
            return {BlendFactor::DstColor, BlendFactor::OneMinusSrcAlpha, BlendFactor::Zero,
                    BlendFactor::One};
        case DrawableBlend::Normal:
        default:
            return {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendFactor::One,
                    BlendFactor::OneMinusSrcAlpha};
    }
}

/// 官方 GetModelColorWithOpacity（premultiplied 路径）：
///   A *= opacity;  R *= A;  G *= A;  B *= A;
/// 只乘 alpha 不缩放 RGB 是本项目「渲染偏白」的根因。
struct ModelColor {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

inline ModelColor ModelColorWithOpacity(float opacity, float modelR = 1.0f,
                                        float modelG = 1.0f, float modelB = 1.0f,
                                        float modelA = 1.0f) {
    const float a = std::min(1.0f, std::max(0.0f, modelA * opacity));
    return {modelR * a, modelG * a, modelB * a, a};
}

/// 遮罩通道（mask pass）写入裁剪缓冲的 baseColor。
///
/// 【为什么恒为不透明白、绝不能乘遮罩源的 opacity】
///   Cubism 的遮罩是「另一块部件的几何形状」，裁剪范围只由三角形覆盖度决定，
///   与遮罩源自己显示得多淡、甚至是否可见都无关。官方 Framework 的
///   CubismClippingManager 在裁剪缓冲里也是用固定白色绘制。
///
///   早期实现把 maskDrawable.opacity 乘进 u_baseColor.a，于是 opacity=0 的
///   遮罩源会把整张遮罩纹理画成全透明 -> 主 pass 采样到的 m=0 ->
///   被裁剪部件整体消失。这类遮罩源在 Cubism 里很常见（纯裁剪部件本身
///   csmIsVisible=false / opacity=0），Q1 之后被隐藏身体下的 drawable
///   opacity 同样被 Core 置 0。典型症状就是「所有 moc3 的眼睛都渲染不出来」。
///
/// @param maskSourceOpacity 遮罩源 drawable 的不透明度。**刻意忽略**：
///        保留参数是为了让「此处不按 opacity 缩放」成为显式、可单测的契约。
inline ModelColor MaskPassColor(float maskSourceOpacity) {
    (void)maskSourceOpacity;
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

/// 遮罩通道是否应绘制该 drawable（纯逻辑契约，可单测）。
///
/// 【Q1 回归】遮罩是「另一块部件的几何形状」，shader 在 u_maskPass 分支
/// 只输出覆盖度、不采样任何纹理，因此**纹理是否就绪一律不影响绘制**。
/// 早期 DrawDrawable() 在 maskPass 里也走「纹理 id 为 0 就 return」，而
/// whiteTexture 只在贴图加载失败时才创建 —— 贴图全部加载成功的模型
/// whiteTexture==0，于是所有遮罩源一个都没画进 FBO（遮罩纹理 alpha 全 0），
/// 被裁剪部件（瞳孔 / 虹膜）被整块裁掉，眼睛变成「框内纯白无眼睛」。
/// textureReady 仅作诊断传入、刻意不参与判定，用于锁定这条回归。
inline bool MaskPassDrawableValid(bool geometryValid, bool textureReady) {
    (void)textureReady;
    return geometryValid;
}

/// 一个 Drawable 的渲染元数据（由 Cubism Core 适配层每帧填充）。
struct DrawableDesc {
    int index = -1;
    bool visible = true;
    float opacity = 1.0f;
    int renderOrder = 0;       ///< 官方 csmGetDrawableRenderOrders（已考虑遮罩关系）
    DrawableBlend blend = DrawableBlend::Normal;
    bool isMaskDrawable = false; ///< 自身被别的部件用作遮罩 → 不画进主画面
    bool hasOwnTexture = true;   ///< 遮罩部件通常没有贴图
    bool useMask = false;        ///< 本部件需要被裁剪
    bool invertMask = false;
    int  maskCount = 0;          ///< 挂在身上的遮罩数量（多遮罩并集用）
};

/// 一条渲染指令：GL 层按顺序执行，不再自己决策。
struct RenderOp {
    int drawableIndex = -1;
    DrawableBlend blend = DrawableBlend::Normal;
    BlendFunc blendFunc;
    ModelColor baseColor;      ///< 送进 u_baseColor（premultiplied）
    bool useMask = false;
    bool invertMask = false;
    int renderOrder = 0;
    int maskCount = 0;
};

/// 构造本帧的渲染指令表。
///
/// @param drawables 所有部件的描述（顺序任意，长度 = DrawableCount）
/// @param modelOpacity 模型整体不透明度（官方 ModelOpacity，默认 1）
/// @return 按 renderOrder 稳定排序、已过滤无效部件的指令表
inline std::vector<RenderOp> BuildRenderPlan(const std::vector<DrawableDesc>& drawables,
                                             float modelOpacity = 1.0f) {
    std::vector<const DrawableDesc*> selected;
    selected.reserve(drawables.size());
    for (const DrawableDesc& d : drawables) {
        if (d.index < 0 || !d.visible || d.opacity <= 0.0f) continue;
        // 【关键修复】绝不能因为「被别的部件当作遮罩源」就剔除。
        // 官方 Cubism Framework 的 DoDrawModel() 会照常绘制遮罩源本身，
        // 只有 csmIsVisible=false / opacity=0 的部件才不画。脸、腿这类
        // 大面积部件常被用作眼睛、发饰、服装的裁剪遮罩，早期版本按
        // isMaskDrawable 一刀切剔除后，这些部位整块消失 → 用户看到的
        // 「脸部和腿部是透明的透明块」。
        // 真正要跳过的只有「无贴图的纯几何遮罩」（画出来是一块白矩形）。
        if (!d.hasOwnTexture) continue;
        selected.push_back(&d);
    }

    // 官方按 RenderOrder 排序；相等时保持原下标顺序（稳定排序），
    // 否则相同层级的部件会随机前后翻转，出现闪烁。
    std::stable_sort(selected.begin(), selected.end(),
                     [](const DrawableDesc* a, const DrawableDesc* b) {
                         if (a->renderOrder != b->renderOrder) {
                             return a->renderOrder < b->renderOrder;
                         }
                         return a->index < b->index;
                     });

    std::vector<RenderOp> plan;
    plan.reserve(selected.size());
    for (const DrawableDesc* d : selected) {
        RenderOp op;
        op.drawableIndex = d->index;
        op.blend = d->blend;
        op.blendFunc = BlendFuncFor(d->blend);
        op.baseColor = ModelColorWithOpacity(d->opacity * modelOpacity);
        op.useMask = d->useMask;
        op.invertMask = d->invertMask;
        op.renderOrder = d->renderOrder;
        op.maskCount = d->maskCount;
        plan.push_back(op);
    }
    return plan;
}

}  // namespace ppet::live2d
