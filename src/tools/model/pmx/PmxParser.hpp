// PPet - C++ 移植版
// PMX (Polygon Model eXtended) 2.0 / 2.1 二进制解析器。
//
// 格式依据（按权威度排序）：
//   1. felixjones 的 PMX 2.1 规范（https://github.com/felixjones/pmx-file-spec）
//      —— 字段语义、骨骼 flag 位定义、SoftBody 段布局。
//   2. three.js 实际使用的 mmd-parser v1.0.0（生产环境验证过的真实 .pmx 解析器）
//      —— 决定二进制读取顺序（尤其骨骼可选段的先后次序）。
//   3. 真实样本 miku.pmx（初音ミク，14590 顶点 / 14987 面 / 122 骨骼）的逐字节核对。
//
// 注意：网络上流传的 "PMX 2.1 spec" 有多个版本，其中 Binsk 的版本描述的其实是
// PMX-Editor 的插件格式（骨骼带 name/name_e 双字段 + connectIndex），
// 与 MMD 的 .pmx 文件格式不同，不能直接照抄。
//
// 设计约束：
//   - 文件来自互联网，可能被截断、篡改或恶意构造。所有读取前必须检查剩余字节，
//     所有 count 字段必须做上限保护，所有索引读入后必须做范围校验。
//   - 本文件不依赖任何第三方库，UTF-16LE -> UTF-8 转换自行实现。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ppet::pmx {

/// 顶点权重类型。
enum class PmxWeightType : std::uint8_t {
    BDef1 = 0,  ///< 单骨骼，权重恒为 1
    BDef2 = 1,  ///< 双骨骼线性混合
    BDef4 = 2,  ///< 四骨骼线性混合
    SDef = 3,   ///< 双骨骼球面混合（本实现降级为 BDef2）
    QDef = 4,   ///< 四骨骼四元数混合（PMX 2.1）
};

/// morph 类型。
enum class PmxMorphType : std::uint8_t {
    Group = 0,    ///< 组 morph（引用其它 morph）
    Vertex = 1,   ///< 顶点形变
    Bone = 2,     ///< 骨骼形变
    Uv = 3,       ///< UV 形变（主 UV）
    UvExt1 = 4,
    UvExt2 = 5,
    UvExt3 = 6,
    UvExt4 = 7,
    Material = 8, ///< 材质形变
    Flip = 9,     ///< 翻转形变（PMX 2.1）
    Impulse = 10, ///< 冲量形变（PMX 2.1）
};

/// 材质 flag 位（真实 PMX 语义，参考 felix 规范 / Material Flags 表）。
enum PmxMaterialFlag : std::uint8_t {
    kMaterialNoCull = 1 << 0,       ///< 双面（关闭背面剔除）
    kMaterialGroundShadow = 1 << 1, ///< 投射地面阴影
    kMaterialDrawShadow = 1 << 2,   ///< 绘制到阴影贴图
    kMaterialReceiveShadow = 1 << 3,///< 接收阴影
    kMaterialHasEdge = 1 << 4,      ///< 启用边缘描边
    kMaterialVertexColor = 1 << 5,  ///< 使用附加 UV1 作为顶点色
    kMaterialPointDraw = 1 << 6,    ///< PMX 2.1 点绘制
    kMaterialLineDraw = 1 << 7,     ///< PMX 2.1 线绘制
};

/// 骨骼 flag 位（真实 PMX 语义）。
enum PmxBoneFlag : std::uint16_t {
    kBoneIndexedTail = 1 << 0,      ///< 尾端用骨骼索引表示（否则为 vec3 偏移）
    kBoneRotatable = 1 << 1,
    kBoneTranslatable = 1 << 2,
    kBoneVisible = 1 << 3,
    kBoneEnabled = 1 << 4,
    kBoneIk = 1 << 5,
    kBoneLocalCoordinate = 1 << 6,
    kBoneInheritRotation = 1 << 8,  ///< 继承旋转
    kBoneInheritTranslation = 1 << 9,///< 继承平移
    kBoneFixedAxis = 1 << 10,       ///< 固定轴
    kBoneLocalAxis = 1 << 11,       ///< 局部轴
    kBonePhysicsAfterDeform = 1 << 12,
    kBoneExternalParentDeform = 1 << 13,
};

/// 一个顶点。
struct PmxVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    /// 附加 UV（最多 4 组，每组 vec4）。
    std::vector<std::array<float, 4>> additionalUvs;

    PmxWeightType weightType = PmxWeightType::BDef1;
    /// 骨骼索引，-1 表示无。
    std::array<int, 4> boneIndices{-1, -1, -1, -1};
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};

    /// SDEF 专用的 C / R0 / R1（本实现降级为 BDef2，仅保留数据）。
    std::array<float, 3> sdefC{};
    std::array<float, 3> sdefR0{};
    std::array<float, 3> sdefR1{};

    /// 描边宽度缩放（PMX 中为「铅笔线粗细」，1.0 约等于 1 像素）。
    float edgeScale = 1.0f;
};

/// 一个材质。
struct PmxMaterial {
    std::string name;
    std::string nameEn;
    std::string comment;

    std::array<float, 4> diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> specular{0.0f, 0.0f, 0.0f};
    float shininess = 5.0f;
    std::array<float, 3> ambient{0.0f, 0.0f, 0.0f};

    std::uint8_t flags = 0;
    std::array<float, 4> edgeColor{0.0f, 0.0f, 0.0f, 1.0f};
    float edgeSize = 0.0f;

    /// 基础色贴图索引，-1 表示无。
    int textureIndex = -1;
    /// 球面/环境贴图索引，-1 表示无。
    int sphereTextureIndex = -1;
    /// 环境贴图混合模式：0 关闭 / 1 相乘 / 2 相加 / 3 附加 UV。
    std::uint8_t sphereMode = 0;

    /// toon 引用方式：0 = 独立 toon 贴图索引，1 = 内置共享 toon（0~9）。
    std::uint8_t toonFlag = 1;
    /// toon 索引。toonFlag==0 时是贴图索引；toonFlag==1 时是内置 toon 序号（0~9）。
    int toonIndex = -1;

    /// 该材质覆盖的「索引数」（不是三角形数！）。
    /// PMX 规范：surface count 恒为 3 的倍数，等于本材质覆盖的顶点索引个数，
    /// 所有材质的该值之和应等于模型的总索引数。
    std::uint32_t faceCount = 0;
};

/// 骨骼的 IK 链条目。
struct PmxIkLink {
    int boneIndex = -1;
    bool hasLimit = false;
    std::array<float, 3> limitMin{};
    std::array<float, 3> limitMax{};
};

/// 一根骨骼。
struct PmxBone {
    std::string name;
    std::string nameEn;

    /// 模型空间绝对坐标（PMX 中骨骼位置是绝对值，不是相对父骨骼的偏移）。
    std::array<float, 3> position{};
    int parentIndex = -1;
    std::uint32_t layer = 0;
    std::uint16_t flags = 0;

    /// 尾端：flags & kBoneIndexedTail 时为骨骼索引，否则为位置偏移。
    int tailIndex = -1;
    std::array<float, 3> tailOffset{};

    /// 继承（付与）：来源骨骼索引与影响率。
    bool hasGrant = false;
    int grantParentIndex = -1;
    float grantRatio = 0.0f;

    /// 固定轴 / 局部轴。
    bool hasFixedAxis = false;
    std::array<float, 3> fixedAxis{};
    bool hasLocalAxis = false;
    std::array<float, 3> localXAxis{};
    std::array<float, 3> localZAxis{};

    /// 外部父级变形 key。
    std::uint32_t externalKey = 0;

    /// IK 信息。
    bool hasIk = false;
    int ikTargetIndex = -1;
    std::uint32_t ikIteration = 0;
    float ikLimitAngle = 0.0f;
    std::vector<PmxIkLink> ikLinks;
};

/// morph 的一个元素（不同类型字段含义不同，按类型读取）。
struct PmxMorphElement {
    /// 顶点 / 骨骼 / morph / 材质索引。
    int index = -1;
    /// 顶点形变与骨骼形变的位移，UV 形变则用 uv。
    std::array<float, 4> offset{};
    /// 骨骼形变的旋转（四元数 xyzw）。
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    /// 组 morph 的影响率。
    float influence = 0.0f;
};

/// 一个 morph。
struct PmxMorph {
    std::string name;
    std::string nameEn;
    std::uint8_t panel = 0;
    PmxMorphType type = PmxMorphType::Vertex;
    std::uint32_t elementCount = 0;
    std::vector<PmxMorphElement> elements;
};

/// 显示帧（表情框）：绑定一组骨骼或一组 morph。
struct PmxDisplayFrame {
    std::string name;
    std::string nameEn;
    std::uint8_t specialFlag = 0;
    /// 目标类型：0 = 骨骼，1 = morph。
    struct Item {
        std::uint8_t target = 0;
        int index = -1;
    };
    std::vector<Item> items;
};

/// 刚体。
struct PmxRigidBody {
    std::string name;
    std::string nameEn;
    int boneIndex = -1;
    std::uint8_t groupId = 0;
    std::uint16_t nonCollisionMask = 0;
    /// 形状：0 球 / 1 盒 / 2 胶囊。
    std::uint8_t shapeType = 0;
    std::array<float, 3> size{};
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    float mass = 1.0f;
    float moveAttenuation = 0.5f;
    float rotationDamping = 0.5f;
    float restitution = 0.0f;
    float friction = 0.5f;
    /// 0 跟随骨骼 / 1 物理运算 / 2 物理运算 + 骨骼。
    std::uint8_t physicsMode = 0;
};

/// 关节（约束）。
struct PmxJoint {
    std::string name;
    std::string nameEn;
    std::uint8_t type = 0;
    int rigidBodyA = -1;
    int rigidBodyB = -1;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    std::array<float, 3> limitMin{};
    std::array<float, 3> limitMax{};
    std::array<float, 3> rotLimitMin{};
    std::array<float, 3> rotLimitMax{};
    std::array<float, 3> springPosition{};
    std::array<float, 3> springRotation{};
};

/// 解析出的完整 PMX 文档。
struct PmxDocument {
    std::string name;
    std::string nameEn;
    std::string comment;
    std::string commentEn;
    float version = 2.0f;

    std::vector<PmxVertex> vertices;
    /// 每个三角形三个顶点索引。
    std::vector<std::array<std::uint32_t, 3>> faces;
    /// 贴图路径（相对 PMX 文件所在目录，未做路径前缀拼接）。
    std::vector<std::string> textures;
    std::vector<PmxMaterial> materials;
    std::vector<PmxBone> bones;
    std::vector<PmxMorph> morphs;
    std::vector<PmxDisplayFrame> displayFrames;
    std::vector<PmxRigidBody> rigidBodies;
    std::vector<PmxJoint> joints;

    /// 顶点索引越界而被丢弃的三角形数量。
    std::uint32_t droppedFaces = 0;

    /// 文件编码：true = UTF-16LE，false = UTF-8。
    bool utf16 = true;
};

/// 解析 PMX 文件。
/// @return 成功返回 true；失败返回 false，error 中写入可读的中文错误信息。
bool ParsePmx(const std::string& path, PmxDocument& out, std::string& error);

}  // namespace ppet::pmx
