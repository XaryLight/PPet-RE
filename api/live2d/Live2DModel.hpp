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
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/Model.hpp>
#include <api/live2d/CubismCore.hpp>
#include <api/live2d/Live2DMotion.hpp>
#include <api/live2d/MotionDirector.hpp>
#include <api/live2d/Stage.hpp>

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

    /// 关键参数的当前值（名称 -> 值）。
    ///
    /// 用于诊断「动画到底有没有在动」：待机动作、物理、表情都会改参数，
    /// 只要采样若干帧对比数值，就能确认哪一层在工作。
    std::vector<std::pair<std::string, float>> ParameterSnapshot() const override;

    /// 用户点击渲染窗口：按命中区触发互动动作（TapHead / TapBody 等）。
    /// nx, ny ∈ [0,1]，左上原点、y 向下。
    bool OnInteract(float nx, float ny) override;

    // --- 控制台调试接口 ---
    std::vector<std::string> MotionNames() const override;
    bool PlayMotion(const std::string& id) override;
    bool StopMotion() override;
    void ReloadRenderer() override;
    std::vector<std::pair<std::string, std::string>> RenderResources() const override;
    float PreferredFrameAspect() const override;

    /// 表情/口型：切换到表情动作组（如 "ExpHappy"）；无对应组返回 false。
    bool SetExpressionPreset(const std::string& id) override;
    /// 口型：设置开口度与嘴形（来自 LipSync），刷入 ParamMouthOpenY/ParamMouthForm。
    bool SetLipSync(float open, float form) override;

    /// 可用的表情预设 id：Live2D 的「表情」是表情类动作组（如 "ExpHappy"）。
    /// 托盘「表情」子菜单据此生成条目。
    std::vector<std::string> ExpressionPresetIds() const override;

    /// 待机随机表情：随机播一个表情动作组（对齐 VRM-VPet 的随机表情）。
    void SetRandomIdleExpression(bool enabled) override;

    /// 设置视线追踪强度倍率（0 = 关闭眼睛跟随，1 = 默认，2 = 双倍）。
    void SetTrackingStrength(float strength);
    float TrackingStrength() const { return trackingStrength_; }
    bool RandomIdleExpression() const override;
    bool TriggerRandomExpression() override;

private:
    /// 下一次随机表情的等待时间（秒）。
    float RandomExpressionWait();

    /// 探测模型里的表情参数并生成小表情预设。
    ///
    /// Live2D 的表情参数有一套事实上的命名惯例（Cubism 官方模板与绝大多数
    /// 模型都遵循），本函数按这些惯例探测模型实际拥有的参数，
    /// 只为**真实存在**的参数生成预设——不存在的参数写进去没有效果，
    /// 反而会掩盖真正的表情能力。
    void ResolveFaceParameters();

    /// 应用待机小表情（叠加到模型参数上，不覆盖动作写入的值）。
    void ApplyIdleFaces(float deltaTime);

    /// 加载纹理。失败时使用白色占位纹理，不中断加载。
    void LoadTextures();

    /// 加载动作文件。
    void LoadMotions();

    /// 载入物理设置（physics3.json）：解析参数映射并把摆锤安定到静止姿态。
    /// 模型没有物理文件时什么都不做（只是没有头发摇曳）。
    void LoadPhysics();

    /// 推进物理并把结果叠加回模型参数。
    ///
    /// 物理的输入通常是头部/身体角度参数，因此必须在视线追踪**之后**调用。
    void ApplyPhysics(float deltaTime);

    /// 建立视线追踪所需的参数索引。参数不存在时索引保持 -1。
    void ResolveTrackingParameters();

    /// 应用视线追踪到模型参数。
    void ApplyTracking(const RenderContext& ctx, float deltaTime);
    /// 应用自动眨眼。
    void ApplyEyeBlink(float deltaTime);

    /// 应用呼吸起伏。
    void ApplyBreathing(float deltaTime);

    /// 推进动作播放并把曲线值写入参数。
    void UpdateMotion(float deltaTime);

    /// 应用口型（开口度 + 嘴形）到模型参数。
    void ApplyMouth(float deltaTime);

    /// 计算模型单位坐标到裁剪空间的变换矩阵。
    Mat4 BuildTransform(const RenderContext& ctx) const;

    /// 解析遮罩关系（部件 -> 遮罩列表，并标出哪些部件本身是遮罩）。
    /// 遮罩关系在模型加载后是静态的，只需要在结构变化时解析一次。
    void ResolveMaskRelations(int drawableCount);

    /// 把遮罩画进遮罩纹理。
    ///
    /// 遮罩用与主画面相同的 MVP 矩阵绘制，因此遮罩纹理与主画面的
    /// 「模型坐标 → 像素」一一对应，片元着色器可直接按裁剪空间采样。
    ///
    /// @return 遮罩纹理不可用时返回 false（调用方应退化为不裁剪）。
    bool RenderMaskTexture(const std::vector<cubism::MaskRef>& masks, const Mat4& transform,
                           const RenderContext& ctx);

    /// 绘制一个部件。
    /// @param maskPass true 表示画进遮罩纹理（此时没有自己的贴图就用白色实心填充）。
    void DrawDrawable(int index, const Mat4& transform, bool maskPass);

    /// 释放所有 GPU 资源。
    void ReleaseGraphics();

    // --- 模型数据 ---
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // --- 状态 ---
    bool loaded_ = false;
    std::string displayName_;

    // --- 待机随机表情 ---
    //
    // 两条通道并行工作，缺一不可：
    //   1) 表情动作组（MotionGroupKind::Expression）：模型有 "Exp*" 组时随机播；
    //   2) 表情参数（IdleFaces）：模型没有表情动作组时，靠 ParamBrowLY /
    //      ParamCheek / ParamEyeLSmile 这类参数做小表情。
    // 很多模型只有第 2 条通道，只做第 1 条会让随机表情对这类模型完全无效。
    bool randomExpression_ = false;
    float randomExpressionTimer_ = 12.0f;
    float randomExpressionHold_ = -1.0f;
    std::mt19937 expressionRng_{20240501u};

    /// 参数级待机小表情（眉毛/脸颊/笑眼）。
    IdleFaces idleFaces_;

    /// 表情参数索引缓存：参数名 -> Cubism 索引（-1 表示模型没有该参数）。
    std::map<std::string, int> faceParamIndex_;

    /// 视线追踪强度倍率（1 = 默认）。可在设置里放大/缩小眼睛跟随幅度。
    float trackingStrength_ = 1.0f;

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

    /// 三通道视线阻尼（眼/头/身体各自指数追向目标，参考 FULASKA 的 GazeChannels）。
    GazeChannels gaze_;

    /// 待机导演：常驻微摆 + 随机成套动作（IdleDirector）。
    IdleDirector idleStage_;

    /// 口型导演：呼吸微动 + 说话音节 + 情绪嘴形（MouthDirector）。
    MouthDirector mouthStage_;

    /// 口型目标（由 SetLipSync 设置，每帧平滑追向它）。
    float talkOpen_ = 0.0f;

    /// 口型参数索引（ParamMouthOpenY / ParamMouthForm），-1 表示模型无此参数。
    int paramMouthOpenY_ = -1;
    int paramMouthForm_ = -1;

    /// 眨眼状态。
    float blinkTimer_ = 0.0f;
    float blinkPhase_ = 0.0f;
    bool blinking_ = false;

    /// 呼吸相位。
    float breathPhase_ = 0.0f;

    /// 动作调度器：Idle 待机随机循环 + 点击互动 + 优先级 + 淡入淡出。
    /// 参考原 ppet / Touhou Desktop Pet 的动作组织，纯逻辑、可单测。
    MotionDirector motionDirector_;

    /// 眨眼随机源（与动作调度分开，互不影响可复现性）。
    std::mt19937 rng_{20240101u};
};

}  // namespace ppet::live2d
