// PPet - C++ 移植版
// 统一蒙皮网格表示：PMX 与 VRM 加载后都转换为此结构，共用一套 toon 渲染管线。
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/Math.hpp"

namespace ppet {

/// 材质。字段同时覆盖 PMX 与 VRM(MToon) 两种来源，缺失的项保持默认值。
struct Material {
    std::string name;

    Vec4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    Vec3 specular{0.0f, 0.0f, 0.0f};
    float shininess = 5.0f;

    // Toon 着色参数（VRM MToon / PMX 卡通光照共用）。
    Vec3 shadeColor{0.3f, 0.3f, 0.3f};
    Vec3 rimColor{0.0f, 0.0f, 0.0f};
    float rimLightingMix = 1.0f;
    float rimFresnelPower = 1.0f;
    float shadeShift = 0.0f;   ///< 阴影偏移 (-1..1)
    float shadeToony = 0.9f;   ///< 阴影集中度 (0=软 1=硬)
    float lightIntensity = 1.0f;

    // 描边（逆时针外扩法线挤出）。
    float outlineWidth = 0.0f;
    Vec3 outlineColor{0.0f, 0.0f, 0.0f};

    // 纹理索引，指向 SkinnedMesh::textures；-1 表示无。
    int baseColorTex = -1;
    int toonRampTex = -1;   ///< PMX 卡通渐变贴图 / VRM shade texture
    int sphereTex = -1;     ///< PMX 球面映射贴图
    int normalTex = -1;

    bool useToon = true;
    bool unlit = false;     ///< KHR_materials_unlit
    bool doubleSided = true;

    /// PMX 的描边以 edge scale 表达，与 VRM 的 width 单位不同，此处归一化到世界单位。
    float edgeScale = 1.0f;
};

/// 蒙皮顶点。
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<std::uint32_t, 4> joints{0, 0, 0, 0};
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};
};

/// 骨骼。
struct Bone {
    std::string name;
    int parent = -1;

    Mat4 localBindPose;      ///< 绑定姿态下的局部变换
    Mat4 inverseBindMatrix;  ///< 骨骼空间的逆绑定矩阵
    Mat4 currentLocal;       ///< 当前局部变换（含动画）
    Mat4 world;              ///< 当前世界变换

    bool hasTranslation = false;  ///< PMX 中该骨骼是否参与位移
    Vec3 restPosition;            ///< 绑定姿态世界坐标（IK 用）

    /// IK 链（PMX IK 骨骼 / VRM 约束近似）。
    int ikTarget = -1;
    int ikIteration = 0;
    float ikLimitAngle = 0.0f;

    Mat4 ComputeWorld() const {
        if (parent < 0) return currentLocal;
        return parentWorld * currentLocal;
    }
    Mat4 parentWorld;
};

/// 骨骼蒙皮网格。
struct SkinnedMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Material> materials;
    std::vector<Bone> bones;

    /// 子网格：索引区间 + 材质索引。
    struct SubMesh {
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount = 0;
        int materialIndex = 0;
    };
    std::vector<SubMesh> subMeshes;

    /// morph 目标：PMX 顶点变形 / VRM 表情。
    struct MorphTarget {
        std::string name;
        std::vector<Vec3> positions;  ///< 与 vertices 等长，未参与该 morph 的顶点为 0
        std::vector<Vec3> normals;
    };
    std::vector<MorphTarget> morphs;

    /// 表情 -> morph 权重映射（VRM expressions）。
    struct Expression {
        std::string name;
        std::string category;   ///< happy / angry / blink / look / aa / ih / ou / ee / oh
        std::vector<std::pair<int, float>> bindings;  ///< morph 索引 + 权重
        bool isBlendShape = true;
    };
    std::vector<Expression> expressions;

    /// 弹簧骨骼链（VRM springBone / PMX 刚体近似）。
    struct SpringBone {
        std::string name;
        int boneIndex = -1;
        int parentBoneIndex = -1;
        Vec3 colliderCenter;
        float colliderRadius = 0.0f;
        float stiffness = 1.0f;
        float dragForce = 0.5f;
        float gravityPower = 0.0f;
        float hitRadius = 0.0f;
    };
    std::vector<SpringBone> springBones;

    /// 头部/视线控制骨（VRM lookAt）。
    int headBoneIndex = -1;
    int leftEyeBoneIndex = -1;
    int rightEyeBoneIndex = -1;

    /// 模型整体包围盒，用于自动取景。
    Vec3 boundsMin{0.0f, 0.0f, 0.0f};
    Vec3 boundsMax{0.0f, 0.0f, 0.0f};
    float modelHeight = 1.0f;

    bool isSkinned = false;
};

/// 计算骨骼世界矩阵与蒙皮矩阵（写入 bone.world 与 skinningMatrices）。
/// @param skinningMatrices  输出，大小与 bones 相同。
void ComputeSkinningMatrices(SkinnedMesh& mesh, std::vector<Mat4>& skinningMatrices);

/// 根据当前骨骼姿态重算蒙皮网格顶点（含 morph 权重）。
/// @param morphWeights  morph 索引 -> 权重。
void ApplyMorphs(SkinnedMesh& mesh, const std::vector<float>& morphWeights,
                 std::vector<Vertex>& outVertices);

/// 计算模型包围盒与高度。
void ComputeBounds(SkinnedMesh& mesh);

}  // namespace ppet
