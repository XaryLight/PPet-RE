// PPet - C++ 移植版
// 统一蒙皮网格的公共运算：世界矩阵累积、蒙皮矩阵、morph 应用、包围盒。
//
// 注意：这些函数的语义是 PMX 与 VRM 后端共用的契约，PMX 后端（src/model/pmx）
// 依赖它们的约定：
//   - 骨骼数组必须按「父在前、子在后」排列（PMX 后端加载时会做拓扑排序），
//     这样逐级累积世界矩阵才正确。
//   - 蒙皮矩阵采用 skinned = worldNow * inverseBind 的标准约定。
//     绑定姿态下 worldNow == worldBind，因此 inverseBind 必须是绑定世界矩阵的逆。
#include "model/SkinnedMesh.hpp"

#include <algorithm>
#include <cmath>

namespace ppet {

void ComputeSkinningMatrices(SkinnedMesh& mesh, std::vector<Mat4>& skinningMatrices) {
    const std::size_t count = mesh.bones.size();
    skinningMatrices.assign(count, Mat4::Identity());

    for (std::size_t i = 0; i < count; ++i) {
        Bone& bone = mesh.bones[i];
        // parentWorld 由父骨骼的 world 推出；父骨骼必须排在前面（见文件头注释）。
        if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < count) {
            bone.parentWorld = mesh.bones[static_cast<std::size_t>(bone.parent)].world;
        } else {
            bone.parentWorld = Mat4::Identity();
        }
        bone.world = bone.ComputeWorld();
        skinningMatrices[i] = bone.world * bone.inverseBindMatrix;
    }
}

void ApplyMorphs(SkinnedMesh& mesh, const std::vector<float>& morphWeights,
                 std::vector<Vertex>& outVertices) {
    outVertices = mesh.vertices;
    if (morphWeights.empty()) return;

    for (std::size_t m = 0; m < mesh.morphs.size() && m < morphWeights.size(); ++m) {
        const float weight = morphWeights[m];
        if (std::fabs(weight) <= kEpsilon) continue;
        const SkinnedMesh::MorphTarget& target = mesh.morphs[m];
        const std::size_t limit = std::min(target.positions.size(), outVertices.size());
        for (std::size_t i = 0; i < limit; ++i) {
            // morph 的 positions 存的是相对原位置的偏移量（未参与的顶点为 0）。
            outVertices[i].position += target.positions[i] * weight;
            if (i < target.normals.size()) {
                outVertices[i].normal += target.normals[i] * weight;
            }
        }
    }
}

void ComputeBounds(SkinnedMesh& mesh) {
    if (mesh.vertices.empty()) {
        mesh.boundsMin = Vec3(0.0f, 0.0f, 0.0f);
        mesh.boundsMax = Vec3(0.0f, 0.0f, 0.0f);
        mesh.modelHeight = 1.0f;
        return;
    }

    Vec3 lo = mesh.vertices[0].position;
    Vec3 hi = mesh.vertices[0].position;
    for (const Vertex& v : mesh.vertices) {
        lo.x = std::min(lo.x, v.position.x);
        lo.y = std::min(lo.y, v.position.y);
        lo.z = std::min(lo.z, v.position.z);
        hi.x = std::max(hi.x, v.position.x);
        hi.y = std::max(hi.y, v.position.y);
        hi.z = std::max(hi.z, v.position.z);
    }
    mesh.boundsMin = lo;
    mesh.boundsMax = hi;
    mesh.modelHeight = std::max(hi.y - lo.y, kEpsilon);
}

}  // namespace ppet
