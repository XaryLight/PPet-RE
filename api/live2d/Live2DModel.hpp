// PPet - C++ 移植版
// Live2D 模型后端：基于 Cubism Core 的 2D 模型实现。
//
// 【与其它后端的关系】
//   PMX / VRM 走三维管线（骨骼 + toon 着色），Live2D 走二维管线：
//   每个「部件」是一块带 UV 的网格，按 Cubism 给出的渲染顺序叠加绘制。
//   本类实现 IModel 接口，因此上层（App）不需要区分模型类型。
//
// 【职责划分】
//   - Live2DManifest：解析 .model3.json（ moc 路径、纹理、动作）
//   - CubismCoreAdapter：唯一与 Cubism Core API 打交道的地方
//   - Live2DMotion：解析 .motion3.json 并做曲线插值
//   - 本文件：模型生命周期 + 参数驱动（视线追踪 / 眨眼 / 呼吸）+ 绘制
//
// 【坐标与混合】
//   - Core for Native 使用与 OpenGL 相同的坐标系（Y 轴向上，原点在模型中心）。
//     模型单位坐标换算到像素的公式见 CubismCore.hpp 的 CanvasInfo 注释。
//   - Live2D 纹理的 UV 原点在**左上**，而 OpenGL 纹理坐标原点在左下，
//     因此采样时需要翻转 V（官方 Cubism 顶点着色器同样做 1.0 - v）。
//   - 桌宠窗口是透明叠加窗口，需要 premultiplied alpha 混合。
#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/Model.hpp>
#include <api/live2d/Live2DMotion.hpp>

namespace ppet::live2d {

/// Live2D 模型后端。
class Live2DModel : public IModel {
public:
    Live2DModel();
    ~Live2DModel() override;

    /// 加载模型。path 可以是 .model3.json、模型目录或 .moc3。
    /// 失败时 error 写入中文原因。
    bool Load(const std::string& path, std::string& error) override;

    /// 每帧更新：视线追踪、眨眼、呼吸、动作播放，然后重算顶点。
    void Update(const RenderContext& ctx) override;

    /// 绘制模型。
    void Render(const RenderContext& ctx) override;

    /// 窗口尺寸变化（Live2D 按画布比例自适应，无需重建资源）。
    void OnResize(int width, int height) override;

    /// 模型显示名。
    std::string DisplayName() const override;

    /// 模型类型。
    ModelType Type() const override { return ModelType::Live2D; }

    /// 是否成功加载。
    bool IsLoaded() const override;

    /// 诊断快照（供 Qt 模型诊断面板）：moc3 / 纹理 / 部件 / 动作信息。
    ModelDebugInfo DebugInfo() const override;

private:
    /// 加载纹理。失败时使用白色占位纹理，不中断加载。
    void LoadTextures();

    /// 加载动作文件。
    void LoadMotions();

    /// 建立视线追踪所需的参数索引。参数不存在时索引保持 -1。
    void ResolveTrackingParameters();

    /// 应用视线追踪到模型参数。
    void ApplyTracking(const RenderContext& ctx);

    /// 应用自动眨眼。
    void ApplyEyeBlink(float deltaTime);

    /// 应用呼吸起伏。
    void ApplyBreathing(float deltaTime);

    /// 推进动作播放并把曲线值写入参数。
    void UpdateMotion(float deltaTime);

    /// 计算模型单位坐标到裁剪空间的变换矩阵。
    Mat4 BuildTransform(const RenderContext& ctx) const;

    /// 绘制一个部件。
    void DrawDrawable(int index, const Mat4& transform);

    /// 释放所有 GPU 资源。
    void ReleaseGraphics();

    // --- 模型数据 ---
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // --- 状态 ---
    bool loaded_ = false;
    std::string displayName_;

    // 视线追踪参数索引（-1 表示该模型没有这个参数）。
    int paramAngleX_ = -1;
    int paramAngleY_ = -1;
    int paramEyeBallX_ = -1;
    int paramEyeBallY_ = -1;
    int paramBodyAngleX_ = -1;
    int paramBodyAngleZ_ = -1;
    int paramEyeLOpen_ = -1;
    int paramEyeROpen_ = -1;
    int paramBreath_ = -1;

    /// 视线追踪的最大偏转角度（度）。由 App 通过 SetMaxTrackingAngle 设置。
    float maxTrackingAngle_ = 30.0f;

    /// 眨眼状态。
    float blinkTimer_ = 0.0f;
    float blinkPhase_ = 0.0f;
    bool blinking_ = false;

    /// 呼吸相位。
    float breathPhase_ = 0.0f;

    /// 动作播放状态。
    struct ActiveMotion {
        const Motion* motion = nullptr;
        float time = 0.0f;
        bool looping = false;
    };
    ActiveMotion motion_;
    float motionCooldown_ = 0.0f;
    std::mt19937 rng_{20240101u};
};

}  // namespace ppet::live2d
