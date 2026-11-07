// PPet - C++ 移植版
// glTF 2.0 / GLB 容器解析器。
//
// 职责边界：
//   本文件只负责「把 glTF 2.0 文件读成结构化数据」，不涉及任何 VRM 语义
//   （humanoid / expressions / springBone 等由 src/model/vrm/VrmModel.cpp 处理）。
//   解析结果是「忠实反映文件内容」的：所有来自文件的索引都保留原值，
//   越界检查只在读取数据时进行，非法索引一律读不出数据（返回 false），
//   而不是在这里悄悄改成别的值。这样上层可以区分「文件里没有」和「文件里有但是坏的」。
//
// 健壮性约定（与 PMX 解析器保持一致）：
//   - 文件来自互联网，可能被截断、篡改或恶意构造。
//   - 所有二进制读取都做边界检查，文件提前结束返回错误而不是崩溃。
//   - count 类字段有绝对上限，且与「剩余字节数 / 最小单元字节数」交叉校验。
//   - base64 data URI 解码校验字符与长度。
//   - 节点树遍历有深度限制，防止恶意文件构造超深嵌套导致栈溢出。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <src/tools/core/Math.hpp>
#include "nlohmann/json.hpp"

namespace ppet::vrm {

/// 内部构建器：GltfDocument 的私有数据只有它可以写入（定义在 .cpp 中）。
struct GltfParserAccess;

/// 四元数（glTF 的 rotation 顺序为 x, y, z, w）转旋转矩阵。
/// 输入未归一化时内部会自动归一化；全零四元数返回单位阵。
Mat4 QuaternionToMat4(const Vec4& xyzw);

/// glTF 组件类型（数值与 glTF 2.0 规范一致）。
enum GltfComponentType : int {
    kGltfByte = 5120,
    kGltfUnsignedByte = 5121,
    kGltfShort = 5122,
    kGltfUnsignedShort = 5123,
    kGltfUnsignedInt = 5125,
    kGltfFloat = 5126,
};

/// accessor 的元素类型。
enum class GltfType {
    Scalar,
    Vec2,
    Vec3,
    Vec4,
    Mat2,
    Mat3,
    Mat4,
};

/// accessor：描述一段顶点 / 索引 / 矩阵数据在 buffer 中的布局。
struct GltfAccessor {
    int bufferView = -1;
    std::size_t byteOffset = 0;
    int componentType = kGltfFloat;
    bool normalized = false;
    std::size_t count = 0;
    GltfType type = GltfType::Scalar;
};

/// bufferView：buffer 中的一段连续数据。
struct GltfBufferView {
    int buffer = -1;
    std::size_t byteOffset = 0;
    std::size_t byteLength = 0;
    std::size_t byteStride = 0;  ///< 0 表示元素紧密排列
};

/// buffer：已加载到内存的数据。
struct GltfBuffer {
    std::string uri;  ///< 原始 uri（data URI 或外部路径），仅用于诊断
    std::vector<unsigned char> data;
};

/// 网格的一个图元（一个三角形批次）。
struct GltfPrimitive {
    int mode = 4;  ///< 4 = TRIANGLES
    int positionAccessor = -1;
    int normalAccessor = -1;
    int texcoordAccessor = -1;
    int tangentAccessor = -1;
    int colorAccessor = -1;
    int jointsAccessor = -1;
    int weightsAccessor = -1;
    int indicesAccessor = -1;
    int material = -1;

    /// morph 目标（相对顶点的位移 / 法线偏移）。
    struct MorphTarget {
        int position = -1;
        int normal = -1;
        int tangent = -1;
    };
    std::vector<MorphTarget> targets;
};

/// 网格。
struct GltfMesh {
    std::string name;
    std::vector<GltfPrimitive> primitives;
    std::vector<float> weights;             ///< morph 默认权重
    std::vector<std::string> targetNames;   ///< 来自 extras.targetNames（VRM 约定）
};

/// 节点（场景图中的一个变换节点）。
struct GltfNode {
    std::string name;
    std::vector<int> children;
    Mat4 local = Mat4::Identity();  ///< 局部变换（TRS 合成或 matrix）
    bool hasMatrix = false;         ///< true 表示 local 来自 matrix 字段
    /// 局部 TRS 的分量形式。动画采样按分量覆盖时需要它们
    /// （把矩阵拆回 TRS 再重新合成，避免把动画值乘进已有矩阵）。
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};  ///< 四元数（xyzw）
    Vec3 scale{1.0f, 1.0f, 1.0f};
    int mesh = -1;
    int skin = -1;
    std::vector<float> weights;  ///< morph 默认权重（节点级覆盖）
};

/// 蒙皮。
struct GltfSkin {
    std::string name;
    std::vector<int> joints;                    ///< 关节节点索引
    int inverseBindMatricesAccessor = -1;
    int skeleton = -1;
};

/// 图片（贴图的像素来源）。
struct GltfImage {
    std::string name;
    std::string mimeType;
    std::string uri;      ///< 外部路径或 data URI
    int bufferView = -1;  ///< GLB 内嵌图片
};

/// 采样器。
struct GltfSampler {
    int magFilter = 9729;   ///< LINEAR
    int minFilter = 9987;   ///< LINEAR_MIPMAP_LINEAR
    int wrapS = 10497;      ///< REPEAT
    int wrapT = 10497;
};

/// 纹理。
struct GltfTexture {
    int source = -1;  ///< images 索引
    int sampler = -1;
    /// KHR_texture_transform
    bool hasTransform = false;
    Vec2 uvOffset{0.0f, 0.0f};
    Vec2 uvScale{1.0f, 1.0f};
    float uvRotation = 0.0f;
};

/// 材质（同时覆盖 glTF PBR 与 VRM MToon 扩展）。
struct GltfMaterial {
    std::string name;
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    int baseColorTexture = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    Vec3 emissiveFactor{0.0f, 0.0f, 0.0f};
    int emissiveTexture = -1;
    int normalTexture = -1;
    int occlusionTexture = -1;
    std::string alphaMode = "OPAQUE";
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;  ///< KHR_materials_unlit

    /// VRMC_materials_mtoon
    bool hasMtoon = false;
    Vec3 shadeColorFactor{0.3f, 0.3f, 0.3f};
    int shadeMultiplyTexture = -1;
    int shadeColorTexture = -1;
    Vec3 rimColorFactor{0.0f, 0.0f, 0.0f};
    int rimMultiplyTexture = -1;
    int rimColorTexture = -1;
    /// 边缘光与光照的混合比例（MToon 的 rimLightingMixFactor）。
    float rimLightingMixFactor = 0.0f;
    /// 描边宽度模式："none" / "worldCoordinates" / "screenCoordinates"。
    std::string outlineWidthMode = "none";
    float outlineWidthFactor = 0.0f;
    Vec3 outlineColorFactor{0.0f, 0.0f, 0.0f};
    float outlineLightingMixFactor = 1.0f;
    float lightColorAttenuation = 0.5f;
    float indirectLightIntensity = 0.1f;
    float reflectanceFactor = 0.5f;
    int reflectanceTexture = -1;
    float clearCoatFactor = 0.0f;
    float clearCoatRoughnessFactor = 0.0f;
    Vec3 sheenColorFactor{0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.5f;
    float shadingShiftFactor = 0.0f;
    float shadingToonyFactor = 0.9f;
    float parametricRimFresnelPowerFactor = 5.0f;
};

/// 动画通道。
struct GltfAnimationChannel {
    int sampler = -1;
    int node = -1;
    std::string path;  ///< translation / rotation / scale / weights
};

/// 动画采样器。
struct GltfAnimationSampler {
    int input = -1;   ///< 时间 accessor
    int output = -1;  ///< 值 accessor
    std::string interpolation = "LINEAR";
};

/// 动画。
struct GltfAnimation {
    std::string name;
    std::vector<GltfAnimationChannel> channels;
    std::vector<GltfAnimationSampler> samplers;
};

/// 一份解析好的 glTF 文档。
///
/// 只提供只读访问；所有修改都在解析阶段完成。
class GltfDocument {
public:
    GltfDocument() = default;
    ~GltfDocument() = default;
    GltfDocument(const GltfDocument&) = delete;
    GltfDocument& operator=(const GltfDocument&) = delete;
    GltfDocument(GltfDocument&&) noexcept = default;
    GltfDocument& operator=(GltfDocument&&) noexcept = default;

    /// 原始 JSON。VRM 扩展（VRMC_vrm 等）直接在这里面。
    const nlohmann::json& Json() const { return json_; }

    const std::vector<GltfNode>& Nodes() const { return nodes_; }
    const std::vector<GltfMesh>& Meshes() const { return meshes_; }
    const std::vector<GltfSkin>& Skins() const { return skins_; }
    const std::vector<GltfMaterial>& Materials() const { return materials_; }
    const std::vector<GltfTexture>& Textures() const { return textures_; }
    const std::vector<GltfImage>& Images() const { return images_; }
    const std::vector<GltfAnimation>& Animations() const { return animations_; }
    const std::vector<int>& SceneRoots() const { return sceneRoots_; }

    /// accessor / bufferView / buffer / sampler 的数量。
    ///
    /// 上层做索引范围校验时需要知道数组长度（例如 VRM 扩展里的
    /// morphTargetBinds.index 到底有没有超出 morph 目标的数量）。
    std::size_t AccessorCount() const { return accessors_.size(); }
    std::size_t BufferViewCount() const { return bufferViews_.size(); }
    std::size_t BufferCount() const { return buffers_.size(); }
    std::size_t SamplerCount() const { return samplers_.size(); }

    /// 读取 accessor 中的浮点数据。
    /// 整数类型且 normalized=true 时会按规范转换到 [-1,1] 或 [0,1]。
    bool ReadAccessorFloats(int accessorIndex, std::vector<float>& out,
                             std::string& error) const;

    /// 读取 accessor 中的 Vec3 数据。
    bool ReadAccessorVec3(int accessorIndex, std::vector<Vec3>& out,
                           std::string& error) const;

    /// 读取 accessor 中的 Vec4 数据。
    bool ReadAccessorVec4(int accessorIndex, std::vector<Vec4>& out,
                           std::string& error) const;

    /// 读取 accessor 中的无符号整数（索引 / 关节号）。
    bool ReadAccessorU32(int accessorIndex, std::vector<std::uint32_t>& out,
                          std::string& error) const;

    /// 读取 accessor 中的 4x4 矩阵（逆绑定矩阵）。
    bool ReadAccessorMat4(int accessorIndex, std::vector<Mat4>& out,
                           std::string& error) const;

    /// 取图片的原始字节（GLB 内嵌或外部文件）。
    /// @param mimeType 输出图片的 MIME 类型（png / jpeg），未知时为空串。
    bool GetImageBytes(int imageIndex, std::vector<unsigned char>& out,
                       std::string& mimeType, std::string& error) const;

    /// 非致命问题（例如被跳过的图元模式）记录，供上层显示。
    const std::vector<std::string>& Warnings() const { return warnings_; }

private:
    friend struct GltfParserAccess;

    nlohmann::json json_;
    std::string baseDir_;  ///< 外部资源的相对目录
    std::vector<GltfBuffer> buffers_;
    std::vector<GltfBufferView> bufferViews_;
    std::vector<GltfAccessor> accessors_;
    std::vector<GltfMesh> meshes_;
    std::vector<GltfNode> nodes_;
    std::vector<GltfSkin> skins_;
    std::vector<GltfMaterial> materials_;
    std::vector<GltfTexture> textures_;
    std::vector<GltfImage> images_;
    std::vector<GltfSampler> samplers_;
    std::vector<GltfAnimation> animations_;
    std::vector<int> sceneRoots_;
    std::vector<std::string> warnings_;
};

/// 加载 .glb（GLB 容器）或 .gltf（JSON + 外部资源）文件。
/// @return 成功返回 true；失败返回 false，error 中写入可读的中文错误信息。
bool LoadGltf(const std::string& path, GltfDocument& out, std::string& error);

}  // namespace ppet::vrm
