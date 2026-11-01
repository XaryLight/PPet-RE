// PPet - C++ 移植版
// PMX 模型后端：把 PMX 文件转换成统一的 SkinnedMesh，并驱动 toon 渲染管线。
//
// 坐标系约定：
//   MMD / PMX 使用左手系（Y 轴向上，模型朝向 -Z）。
//   本渲染管线统一使用右手系（OpenGL 惯例，Y 轴向上，模型朝向 +Z）。
//   因此载入时执行「左手系 -> 右手系」转换：
//     1. 所有 z 分量取反（位置与法线）；
//     2. 三角形顶点顺序反转（0 <-> 2），以抵消镜像导致的绕序翻转。
//   依据：实测 miku.pmx 14987 个三角形中，原始顶点顺序下叉积与顶点法线同向占
//   99.5%；仅取反 z 后同向率降到 0.5%；取反 z 并交换 0/2 后重新回到 99.5%。
//   这正是 three.js 的 mmd-parser 中 leftToRightModel() 所做的变换。
//   另外，实测模型文件的「左手臂」位于 +X，与 MMD 中「模型朝 -Z、屏幕右为 +X」
//   的表现一致，因此取反 z 后从 +Z 方向观察，屏幕成像与 MMD 完全相同（不会镜像）。
//
// 骨骼约定：
//   PMX 中骨骼 position 是「模型空间绝对坐标」，而非相对父骨骼的偏移。
//   实测依据：miku.pmx 首根骨骼 y=16.53，与其绑定顶点包围盒中心 y=16.84 吻合；
//   若按相对父级累加会得到 37.83，明显越界。
//   PMX 骨骼没有旋转字段，绑定姿态下骨骼变换只有平移，因此：
//     localBindPose      = Translation(bonePos - parentBonePos)
//     世界绑定矩阵        = Translation(bonePos)
//     inverseBindMatrix  = 世界绑定矩阵的逆
//   这与 MMD 自身的蒙皮绑定方式一致（MMD 的骨骼世界矩阵在静止姿态下也只有平移）。
//
// 拓扑排序：
//   PMX 允许 parentIndex 大于自身索引（父骨骼排在子骨骼之后），
//   甚至存在环。因此这里用拓扑排序重排骨骼，并对环做安全截断。
//   重排后骨骼数组满足「父在前、子在后」，这样逐顺序遍历即可算出世界矩阵。
#pragma once

#include <string>
#include <vector>

#include "core/Math.hpp"
#include "model/Model.hpp"
#include "model/SkinnedMesh.hpp"
#include "model/pmx/PmxParser.hpp"
#include "render/Renderer.hpp"

namespace ppet::pmx {

/// PMX 模型后端：解析 PMX 文件，转换成 SkinnedMesh 并渲染。
///
/// 线程约束：所有成员函数都在渲染线程调用，无需加锁。
class PmxModel : public IModel {
public:
    PmxModel() = default;
    ~PmxModel() override = default;

    /// 加载 PMX 文件。成功返回 true，失败时 error 写入中文错误信息。
    /// 注意：本函数不做任何 GPU 操作（纹理解码与 VBO 上传需要有效的 OpenGL 上下文），
    /// 因此可以在没有 GL 上下文的环境下安全调用；GPU 资源在首次 Render() 时建立。
    bool Load(const std::string& path, std::string& error) override;

    /// 每帧更新：待机动画 + 视线追踪。
    void Update(const RenderContext& ctx) override;

    /// 绘制。
    void Render(const RenderContext& ctx) override;

    /// 窗口尺寸变化（用于重新计算取景）。
    void OnResize(int width, int height) override;

    /// 显示名：优先用 PMX 内的模型名，缺失时退回文件名。
    std::string DisplayName() const override;

    ModelType Type() const override { return ModelType::Pmx; }

    bool IsLoaded() const override { return loaded_; }

    /// 供测试与调试使用的只读访问。
    const SkinnedMesh& Mesh() const { return mesh_; }

private:
    /// PMX 文档 -> SkinnedMesh。
    void BuildMesh();
    /// 加载贴图路径（拼上 PMX 所在目录），不做 GPU 操作。
    void PrepareTexturePaths(const std::string& baseDir);
    /// 首次渲染时建立 GPU 资源（上传网格 + 解码贴图）。
    void EnsureGpuResources();
    /// 寻找头部 / 左右眼 / 躯干骨骼，找不到时保持 -1（优雅降级）。
    void FindControlBones();
    /// 计算视线追踪目标角度并做平滑插值。
    void UpdateGaze(const RenderContext& ctx, float dt);
    /// 待机动画（呼吸 / 轻微摆动），写入骨骼的 currentLocal。
    void UpdateIdle(float dt);
    /// 把动画结果写入骨骼局部变换（先复位为绑定姿态，再叠加待机与视线旋转）。
    void ApplyBoneTransforms();
    /// 按当前骨骼姿态重算蒙皮矩阵。
    void UpdateSkinningMatrices();
    /// 计算视图投影矩阵：正交相机，按模型高度自动取景并居中。
    Mat4 ComputeViewProjection(int viewportWidth, int viewportHeight,
                                float renderScale = 1.0f) const;

    /// 原始 PMX 文档。
    PmxDocument doc_;
    /// 转换后的统一网格。
    SkinnedMesh mesh_;
    /// 渲染器。
    SkinnedMeshRenderer renderer_;

    /// 已加载的贴图（与 mesh_.materials 中的纹理索引对应）。
    std::vector<Texture> textures_;
    /// 贴图文件路径（相对 PMX 文件所在目录拼接后的完整路径）。
    std::vector<std::string> texturePaths_;

    /// 蒙皮矩阵缓存。
    std::vector<Mat4> skinningMatrices_;

    /// mesh_.bones[i] 对应的 PMX 原始骨骼下标（拓扑排序后与原顺序不同）。
    std::vector<int> boneSource_;
    /// PMX morph 下标 -> mesh_.morphs 下标（-1 表示该 morph 未被保留）。
    std::vector<int> morphSource_;

    /// 视线追踪目标骨骼索引，-1 表示未找到。
    int headBone_ = -1;
    int leftEyeBone_ = -1;
    int rightEyeBone_ = -1;
    /// 躯干骨骼（用于待机摆动），-1 表示未找到。
    int bodyBone_ = -1;

    /// 累计动画时间。
    float time_ = 0.0f;
    /// 当前视线角度（弧度），带平滑插值。
    float gazeYaw_ = 0.0f;
    float gazePitch_ = 0.0f;
    float targetGazeYaw_ = 0.0f;
    float targetGazePitch_ = 0.0f;

    /// 待机动画当前角度（弧度）。
    float idleYaw_ = 0.0f;
    float idlePitch_ = 0.0f;
    /// 待机动画累计时间（独立于 time_，便于单独调节节奏）。
    float idleTime_ = 0.0f;

    /// 模型中心与包围盒高度，用于自动取景。
    Vec3 center_{0.0f, 0.0f, 0.0f};
    float modelHeight_ = 1.0f;

    /// 模型文件路径（用于显示名回退与贴图目录拼接）。
    std::string path_;
    /// 最近一次 OnResize 的窗口尺寸。
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;

    bool loaded_ = false;
    bool gpuReady_ = false;
    /// 是否有材质开启了描边。
    bool hasOutline_ = false;
    /// PMX 材质里是否有材质启用了「顶点色」（顶点色存在附加 UV1 中）。
    bool hasVertexColor_ = false;
};

}  // namespace ppet::pmx
