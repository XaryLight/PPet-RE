// PPet - C++ 移植版
// VRM 1.0 / VRM 0.x 模型后端：把 VRM（GLB 容器 + VRM 扩展）转换成统一的 SkinnedMesh。
//
// 坐标系与朝向（重要，务必与渲染层保持一致）：
//   - glTF / VRM 1.0 使用右手系、Y 轴向上、模型正面朝 +Z、单位为米。
//     依据：glTF 2.0 规范定义 "+Y as up, +Z as forward"；
//     官方样例 VRM1_Constraint_Twist_Sample.vrm 中 leftEye 的 X 为 +、
//     rightEye 的 X 为 -（模型自身的左手在 +X 侧），与「正面朝 +Z」一致。
//   - VRM 0.x 沿用 Unity 约定，模型正面朝 -Z。
//     依据：样例 alicia.vrm 中 LeftUpLeg 的 X 为 -、LeftToeBase 的 Z 为 -（脚尖朝 -Z）。
//     因此载入 VRM 0.x 时统一绕 Y 轴旋转 180°，让两种版本都朝 +Z，
//     渲染层的相机可以固定放在 +Z 侧。
//
// 蒙皮约定（与 core/SkinnedMesh.hpp 的契约一致）：
//   - 骨骼数组按「父在前、子在后」排列，ComputeSkinningMatrices 依赖这一点。
//   - skinningMatrices[i] = bone.world * bone.inverseBindMatrix。
//     glTF 的逆绑定矩阵是「关节绑定姿态世界矩阵的逆」，
//     即 B = inverse(JointWorld_bind)，因此在绑定姿态下
//     bone.world * bone.inverseBindMatrix == 单位阵。
//   - bone.world 是关节节点在模型空间（场景根节点空间）的全局变换，
//     包含场景根节点自身的 TRS——这一点很容易写错，导致模型整体偏移。
//   - 顶点位于模型空间：顶点坐标 = 模型朝向变换 * 网格节点全局变换 * 顶点局部坐标。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/ExpressionDirector.hpp>
#include <src/tools/model/Model.hpp>
#include <src/tools/model/SkinnedMesh.hpp>
#include <src/tools/render/Renderer.hpp>

namespace ppet::vrm {

class GltfDocument;

/// VRM 模型后端。
///
/// 线程约束：所有成员函数都在渲染线程调用，无需加锁。
class VrmModel : public IModel {
public:
    VrmModel();
    ~VrmModel() override;

    /// 加载 VRM 文件。成功返回 true，失败时 error 写入中文错误信息。
    /// 注意：本函数不做任何 GPU 操作（图片解码与 VBO 上传需要有效的 OpenGL 上下文），
    /// 因此可以在没有 GL 上下文的环境下安全调用；GPU 资源在首次 Render() 时建立。
    bool Load(const std::string& path, std::string& error) override;

    /// 每帧更新：待机动画 / 视线追踪 / 眨眼 / 弹簧骨。
    void Update(const RenderContext& ctx) override;

    /// 绘制。
    void Render(const RenderContext& ctx) override;

    /// 窗口尺寸变化（用于重新计算取景）。
    void OnResize(int width, int height) override;

    /// 显示名：优先用 VRM meta.name，缺失时退回文件名。
    std::string DisplayName() const override;

    ModelType Type() const override { return ModelType::Vrm; }

    bool IsLoaded() const override { return loaded_; }

    /// 诊断快照（调试面板）：网格规模、VRM 版本、内嵌贴图数、渲染状态。
    ModelDebugInfo DebugInfo() const override;

    /// 供测试与调试使用的只读访问。
    const SkinnedMesh& Mesh() const { return mesh_; }

    /// 表情权重（与 mesh_.expressions 一一对应），供 UI 调节。
    const std::vector<float>& ExpressionWeights() const { return expressionWeights_; }

    /// 按名字设置某个表情的权重（找不到对应表情时忽略）。
    void SetExpressionWeight(const std::string& name, float weight);

    /// 口型叠加层：把元音权重（aa/ih/ou/ee/oh）写入口型层。
    ///
    /// 为什么不用 ExpressionDirector::PreviewWeights：那个接口会清空全部基础权重，
    /// 每帧写口型就会把用户选的「开心 / 生气」表情抹掉。口型应当是
    /// 「叠在当前表情之上」的一层，这里单独保存，在 Update 里与导演输出合并。
    bool SetExpressionWeights(const std::vector<std::pair<std::string, float>>& weights) override;

    /// 表情预设切换（"happy" / "angry" …）。内部做别名解析，
    /// 所以中文/日文 morph 名（开心 / 笑い）也能命中。
    bool SetExpressionPreset(const std::string& id) override;

    /// 可用的表情预设 id（托盘「表情」子菜单据此生成条目）。
    std::vector<std::string> ExpressionPresetIds() const override;

    /// VRM 版本：1 = VRM 1.0，0 = VRM 0.x，-1 = 未识别出 VRM 扩展。
    int VrmVersion() const { return vrmVersion_; }

private:
    // --- 构建 -------------------------------------------------------------
    /// VRM / glTF -> SkinnedMesh。失败返回 false。
    bool BuildMesh(std::string& error);
    /// 计算每个节点的全局变换（含场景根节点的变换），带环检测与深度限制。
    void BuildSceneGraph();
    /// 收集骨骼节点（关节 + 弹簧骨 + 它们的所有祖先），并按「父在前」排序。
    void CollectBoneNodes();
    /// 读取 humanoid 骨骼映射，得到头 / 左右眼 / 躯干骨骼。
    void BuildHumanoid();
    /// 读取 VRM 表情（VRM 1.0 的 expressions 或 VRM 0.x 的 blendShapeMaster）。
    void BuildExpressions();
    /// 把真实表情通道绑定到 ExpressionDirector（命名空间 ppet 的表情调度器），
    /// 让「预设 / 过渡 / 自动眨眼 / 动作叠层」统一由此驱动。
    void BindExpressionDirector();
    /// 收集节点的所有后代（先序），受深度与数量限制，防止恶意文件构造超深子树。
    void CollectDescendants(int node, std::vector<int>& out) const;
    /// 判断 ancestor 是否是 node 的祖先（含 node 自身）。
    bool IsAncestorNode(int ancestor, int node) const;
    /// 读取弹簧骨定义（VRMC_springBone 或 VRM 0.x 的 secondaryAnimation）。
    void BuildSpringBones();
    /// 把 VRM 扩展里的弹簧骨定义解析成统一的链条描述。
    ///
    /// VRM 1.0 与 VRM 0.x 的写法差异很大（joints[] vs bones[]、
    /// gravityDir 是数组还是对象、碰撞体是下标还是内联对象），
    /// 这里统一成同一种结构，让「骨骼集合的收集」与「弹簧骨的构建」
    /// 共用同一份数据。
    void CollectSpringChains();
    /// 读取 glTF 动画（作为待机动画使用）。
    void BuildAnimations();
    /// 读取材质并建立贴图列表。
    void BuildMaterials();
    /// 读取网格（顶点 / 索引 / 子网格）。
    bool BuildMeshes(std::string& error);
    /// 建立 morph 目标（顶点位移）。
    void BuildMorphs();

    // --- 运行时 -----------------------------------------------------------
    /// 应用 glTF 动画采样结果到骨骼。
    void UpdateAnimation(float dt);
    /// 待机动画（没有 glTF 动画时的呼吸 / 摆动）。
    void UpdateIdle(float dt);
    /// 视线追踪：驱动头部与眼球骨骼。
    void UpdateGaze(const RenderContext& ctx, float dt);
    /// 自动眨眼：驱动 blink 表情。
    void UpdateBlink(float dt);
    /// 弹簧骨物理（verlet + 碰撞球）。
    void UpdateSpringBones(float dt);
    /// 根据表情权重计算 morph 权重，必要时重建顶点数据。
    void UpdateMorphWeights();
    /// 按当前姿态重算蒙皮矩阵。
    void UpdateSkinningMatrices();
    /// 首次渲染时建立 GPU 资源（上传网格 + 解码贴图）。
    void EnsureGpuResources();
    /// 计算视图投影矩阵：正交相机，按模型高度自动取景并居中。
    Mat4 ComputeViewProjection(int viewportWidth, int viewportHeight,
                                float renderScale = 1.0f) const;

    /// 动画通道的运行时数据。
    struct AnimChannel {
        int node = -1;      ///< 目标节点
        int bone = -1;      ///< 对应的骨骼下标（-1 表示不是骨骼，动画被忽略）
        int path = 0;       ///< 0=translation 1=rotation 2=scale 3=weights
        std::vector<float> times;
        std::vector<float> values;  ///< 每个关键帧的输出值（按 components 分组）
        int components = 0;         ///< 每帧分量数（3 / 4 / morph 数量）
        int morphBase = -1;         ///< weights 通道对应的 morph 起始下标
        int interpolation = 0;      ///< 0=LINEAR 1=STEP 2=CUBICSPLINE
    };

    /// 按时间采样一条动画通道，结果写入 out（分量数为 channel.components）。
    /// out 的长度必须 >= channel.components。
    void SampleChannel(const AnimChannel& channel, float time, float* out) const;

    /// 网格节点的 primitive 顶点区间（构建 morph 目标时需要）。
    struct MeshPrimitiveRange {
        int nodeIndex = -1;
        int meshIndex = -1;
        int primitiveIndex = -1;
        std::uint32_t vertexBase = 0;
        std::uint32_t vertexCount = 0;
    };

    /// 节点的 morph 区间（glTF 动画的 weights 通道需要）。
    struct NodeMorphRange {
        int node = -1;
        int base = 0;
        int count = 0;
    };

    /// 弹簧骨的运行时状态。
    struct SpringState {
        Vec3 position;   ///< 当前世界坐标
        Vec3 previous;   ///< 上一帧世界坐标（verlet 用）
        float restLength = 0.0f;  ///< 与父骨骼的距离（绑定姿态）
        Vec3 gravityDir{0.0f, -1.0f, 0.0f};  ///< 世界空间的重力方向
    };

    /// 一条弹簧骨链的统一描述（VRM 1.0 与 0.x 共用）。
    ///
    /// 两种版本扩展的写法差异很大：
    ///   - VRM 1.0：springs[].joints[] 逐关节描述，colliderGroups[].colliders
    ///     是 colliders 数组的下标，gravityDir 是数组。
    ///   - VRM 0.x：secondaryAnimation.boneGroups[] 用 bones[] 描述链条，
    ///     参数在 boneGroups 这一层，gravityDir 是 {x,y,z} 对象。
    /// 这里把两者归一化，参数优先级为「关节级 > 链级」。
    struct SpringChain {
        std::string name;
        /// 链条上的节点（按从根到尖端的顺序）。
        std::vector<int> nodes;
        /// 每个关节的刚度（与 nodes 一一对应）。
        std::vector<float> stiffness;
        /// 每个关节的阻力（与 nodes 一一对应）。
        std::vector<float> dragForce;
        /// 每个关节的重力强度（与 nodes 一一对应）。
        std::vector<float> gravityPower;
        /// 每个关节的碰撞半径（与 nodes 一一对应）。
        std::vector<float> hitRadius;
        /// 每个关节的重力方向（模型空间，与 nodes 一一对应）。
        std::vector<Vec3> gravityDir;
        /// 生效的碰撞体（colliders 数组下标）。
        std::vector<int> colliders;
    };

    /// 弹簧骨碰撞体。
    struct SpringCollider {
        /// 碰撞体所在的节点（-1 表示无效）。
        int node = -1;
        /// 相对节点局部坐标的偏移。
        Vec3 offset;
        /// 球半径。
        float radius = 0.0f;
    };

    /// 原始 glTF 文档。
    std::unique_ptr<GltfDocument> doc_;
    /// 转换后的统一网格。
    SkinnedMesh mesh_;
    /// 渲染器。
    SkinnedMeshRenderer renderer_;

    /// 贴图原始数据（与贴图列表一一对应，GLB 内嵌图片在这里）。
    std::vector<std::vector<unsigned char>> textureBytes_;
    /// 已上传 GPU 的贴图。
    std::vector<Texture> textures_;

    /// 蒙皮矩阵缓存。
    std::vector<Mat4> skinningMatrices_;
    /// morph 之前的原始顶点（ApplyMorphs 会累加偏移，需要保留基准值）。
    std::vector<Vertex> baseVertices_;
    /// morph 权重（与 mesh_.morphs 一一对应）。
    std::vector<float> morphWeights_;
    /// 动画通道写入的 morph 权重（与表情权重叠加，动画优先）。
    std::vector<float> animMorphWeights_;
    /// 表情权重（与 mesh_.expressions 一一对应）。
    std::vector<float> expressionWeights_;
    /// 表情是否为二元表情（VRM 1.0 的 isBinary）。
    /// 二元表情只有 0 / 1 两种状态（例如 blink），权重需要量化而不是线性插值。
    std::vector<char> expressionBinary_;
    /// 口型叠加层（与 mesh_.expressions 一一对应）：说话时临时加在表情之上。
    std::vector<float> mouthOverlay_;
    /// 上一次上传顶点时使用的 morph 权重（用于判断是否需要重新上传）。
    std::vector<float> uploadedMorphWeights_;

    // --- 节点图 -----------------------------------------------------------
    /// 节点 -> 父节点（-1 表示根）。
    std::vector<int> nodeParent_;
    /// 节点 -> 全局变换（模型空间，含场景根节点的变换）。
    std::vector<Mat4> nodeGlobal_;
    /// 节点 -> 在场景图中的深度。
    std::vector<int> nodeDepth_;
    /// 节点 -> 骨骼下标（-1 表示不是骨骼）。
    std::vector<int> boneOfNode_;
    /// 节点 -> morph 起始下标（-1 表示该节点没有 morph）。
    std::vector<int> morphBaseOfNode_;
    /// glTF mesh 索引 -> 该 mesh 在各个引用节点上建立的 morph 段起始下标列表。
    ///
    /// VRM 0.x 的 blendShapeGroups 用 (mesh, index) 引用 morph 目标，
    /// 其中 mesh 是 glTF 的 mesh 下标，index 是该 mesh 内的 morph 目标下标。
    /// 同一个 mesh 被多个节点引用时，每个节点都会建立独立的 morph 段
    /// （顶点在全局空间里的区间不同），表情要同时驱动这些段，
    /// 所以这里存的是「下标列表」而不是单个下标。
    std::vector<std::vector<int>> morphBasesOfMesh_;
    /// 各网格节点的 primitive 顶点区间。
    std::vector<MeshPrimitiveRange> meshRanges_;
    /// 各节点的 morph 区间。
    std::vector<NodeMorphRange> nodeMorphRanges_;
    /// 骨骼 -> 节点下标。
    std::vector<int> nodeOfBone_;
    /// 骨骼 -> 节点原始的局部绑定变换（不含模型朝向变换）。
    ///
    /// 弹簧骨要用它算「骨骼相对父骨骼的偏移方向」，
    /// 而 bone.localBindPose 对链根骨骼来说是模型空间的全局变换，两者含义不同。
    std::vector<Mat4> boneLocalBind_;
    /// 骨骼 -> 节点局部变换前置的模型空间前缀。
    ///
    /// 链根骨骼（父骨骼为 -1）的局部变换在契约里直接被当成世界矩阵使用，
    /// 所以必须带上「模型朝向变换 * 父节点全局变换」这一段前缀；
    /// 其余骨骼的前缀是单位阵。动画重建局部变换时要用它补上前缀。
    std::vector<Mat4> boneModelPrefix_;

    /// 模型朝向变换：VRM 0.x 绕 Y 轴旋转 180°，VRM 1.0 为单位阵。
    Mat4 modelTransform_ = Mat4::Identity();
    /// VRM 版本（1 / 0 / -1）。
    int vrmVersion_ = -1;
    /// VRM meta.name。
    std::string metaName_;

    // --- 动画 -------------------------------------------------------------
    std::vector<AnimChannel> animChannels_;
    float animTime_ = 0.0f;
    float animDuration_ = 0.0f;
    bool hasAnimation_ = false;

    // --- 弹簧骨 -----------------------------------------------------------
    std::vector<SpringState> springStates_;
    /// 解析后的弹簧骨链条（VRM 1.0 / 0.x 归一化后的结果）。
    std::vector<SpringChain> springChains_;
    /// 解析后的弹簧骨碰撞体（springChains_ 里的下标指向这里）。
    std::vector<SpringCollider> springColliders_;

    // --- 控制骨骼 ---------------------------------------------------------
    int headBone_ = -1;
    int leftEyeBone_ = -1;
    int rightEyeBone_ = -1;
    int bodyBone_ = -1;
    /// 视线角度（弧度），带平滑插值。
    float gazeYaw_ = 0.0f;
    float gazePitch_ = 0.0f;
    float targetGazeYaw_ = 0.0f;
    float targetGazePitch_ = 0.0f;
    /// 视线角度上限（弧度），来自 VRM lookAt 的 rangeMap。
    float lookYawLimit_ = 0.0f;
    float lookPitchLimit_ = 0.0f;
    /// 待机动画参数。
    float idleTime_ = 0.0f;
    float idleYaw_ = 0.0f;
    float idlePitch_ = 0.0f;

    // --- 眨眼 -------------------------------------------------------------
    /// 距离下一次眨眼的剩余时间。
    float blinkTimer_ = 0.0f;
    /// 当前眨眼的进度（0 = 未眨眼，1 = 闭眼）。
    float blinkValue_ = 0.0f;
    /// 是否正在眨眼。
    bool blinking_ = false;
    /// 眨眼总时长（秒）。
    float blinkDuration_ = 0.15f;

    /// 模型中心与高度，用于自动取景。
    Vec3 center_{0.0f, 0.0f, 0.0f};
    float modelHeight_ = 1.0f;

    /// 模型文件路径。
    std::string path_;
    /// 最近一次 OnResize 的窗口尺寸。
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;

    /// 表情调度器：把 VRM 的真实表情通道与「预设/过渡/眨眼/动作叠层」解耦。
    /// 驱动 expressionWeights_（每帧 Tick 后同步），由 UpdateMorphWeights 写成 morph。
    ppet::ExpressionDirector expressionDirector_;

    bool loaded_ = false;
    bool gpuReady_ = false;
    /// 是否有材质开启了描边。
    bool hasOutline_ = false;
    /// morph 权重是否发生变化（需要重新上传顶点）。
    bool morphDirty_ = false;
};

}  // namespace ppet::vrm
