// PPet - C++ 移植版
// 用真实模型样本验证 VRM 解析 + 转换的正确性（可作为回归基准）。
//
// 用法：vrm_dump <模型路径>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <src/tools/model/SkinnedMesh.hpp>
#include <src/tools/model/vrm/GltfParser.hpp>
#include <src/tools/model/vrm/VrmModel.hpp>

using namespace ppet;
using namespace ppet::vrm;

namespace {

void DumpDocument(const std::string& path) {
    GltfDocument doc;
    std::string error;
    if (!LoadGltf(path, doc, error)) {
        std::printf("解析失败: %s\n", error.c_str());
        return;
    }
    std::printf("=== glTF 文档 ===\n");
    std::printf("nodes=%zu meshes=%zu skins=%zu materials=%zu textures=%zu images=%zu animations=%zu\n",
                doc.Nodes().size(), doc.Meshes().size(), doc.Skins().size(), doc.Materials().size(),
                doc.Textures().size(), doc.Images().size(), doc.Animations().size());
    std::printf("accessors=%zu bufferViews=%zu buffers=%zu samplers=%zu\n", doc.AccessorCount(),
                doc.BufferViewCount(), doc.BufferCount(), doc.SamplerCount());
    std::printf("场景根节点数=%zu\n", doc.SceneRoots().size());
    for (const auto& warning : doc.Warnings()) {
        std::printf("  警告: %s\n", warning.c_str());
    }
}

void DumpMesh(const std::string& path) {
    VrmModel model;
    std::string error;
    if (!model.Load(path, error)) {
        std::printf("加载失败: %s\n", error.c_str());
        return;
    }
    const SkinnedMesh& mesh = model.Mesh();
    std::printf("=== 转换后的 SkinnedMesh ===\n");
    std::printf("VRM 版本=%d 显示名=%s\n", model.VrmVersion(), model.DisplayName().c_str());
    std::printf("vertices=%zu indices=%zu materials=%zu bones=%zu subMeshes=%zu morphs=%zu "
                "expressions=%zu springBones=%zu\n",
                mesh.vertices.size(), mesh.indices.size(), mesh.materials.size(), mesh.bones.size(),
                mesh.subMeshes.size(), mesh.morphs.size(), mesh.expressions.size(),
                mesh.springBones.size());
    std::printf("bounds=(%.3f %.3f %.3f)..(%.3f %.3f %.3f) height=%.3f\n", mesh.boundsMin.x,
                mesh.boundsMin.y, mesh.boundsMin.z, mesh.boundsMax.x, mesh.boundsMax.y,
                mesh.boundsMax.z, mesh.modelHeight);
    std::printf("head=%d leftEye=%d rightEye=%d\n", mesh.headBoneIndex, mesh.leftEyeBoneIndex,
                mesh.rightEyeBoneIndex);

    // 拓扑校验：父骨骼必须排在子骨骼之前（ComputeSkinningMatrices 的前提）。
    int bad = 0;
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].parent >= 0 && static_cast<std::size_t>(mesh.bones[i].parent) >= i) {
            ++bad;
        }
    }
    std::printf("拓扑错误(父下标>=自身)的骨骼数 = %d\n", bad);

    // 索引合法性。
    std::size_t badIndex = 0;
    for (std::uint32_t idx : mesh.indices) {
        if (idx >= mesh.vertices.size()) ++badIndex;
    }
    std::printf("越界索引数 = %zu\n", badIndex);

    // 权重合法性。
    std::size_t badWeight = 0;
    for (const Vertex& v : mesh.vertices) {
        float sum = 0.0f;
        for (float w : v.weights) sum += w;
        if (std::fabs(sum - 1.0f) > 1e-3f) ++badWeight;
    }
    std::printf("权重和不为 1 的顶点数 = %zu\n", badWeight);

    // 子网格覆盖检查。
    std::size_t covered = 0;
    for (const SkinnedMesh::SubMesh& s : mesh.subMeshes) covered += s.indexCount;
    std::printf("子网格覆盖索引 = %zu / 总索引 = %zu\n", covered, mesh.indices.size());

    // 绑定姿态下的蒙皮矩阵应当接近单位阵。
    std::vector<Mat4> skin;
    ComputeSkinningMatrices(const_cast<SkinnedMesh&>(mesh), skin);
    double maxDeviation = 0.0;
    for (const Mat4& m : skin) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                const float expected = (r == c) ? 1.0f : 0.0f;
                maxDeviation = std::max(maxDeviation,
                                        static_cast<double>(std::fabs(m.At(r, c) - expected)));
            }
        }
    }
    std::printf("蒙皮矩阵数 = %zu，绑定姿态最大偏差 = %.6f\n", skin.size(), maxDeviation);

    if (mesh.headBoneIndex >= 0) {
        const Bone& head = mesh.bones[static_cast<std::size_t>(mesh.headBoneIndex)];
        std::printf("头部骨骼: %s (parent=%d)\n", head.name.c_str(), head.parent);
    }
    if (mesh.leftEyeBoneIndex >= 0) {
        std::printf("左眼骨骼: %s\n",
                    mesh.bones[static_cast<std::size_t>(mesh.leftEyeBoneIndex)].name.c_str());
    }
    if (mesh.rightEyeBoneIndex >= 0) {
        std::printf("右眼骨骼: %s\n",
                    mesh.bones[static_cast<std::size_t>(mesh.rightEyeBoneIndex)].name.c_str());
    }

    // 表情与弹簧骨概览。
    std::printf("表情 (%zu 个):\n", mesh.expressions.size());
    for (std::size_t i = 0; i < mesh.expressions.size() && i < 12; ++i) {
        std::printf("  [%zu] %s (category=%s, 绑定数=%zu)\n", i, mesh.expressions[i].name.c_str(),
                    mesh.expressions[i].category.c_str(), mesh.expressions[i].bindings.size());
    }
    if (mesh.expressions.size() > 12) std::printf("  ... 其余 %zu 个省略\n", mesh.expressions.size() - 12);
    std::printf("弹簧骨 (%zu 个):\n", mesh.springBones.size());
    for (std::size_t i = 0; i < mesh.springBones.size() && i < 8; ++i) {
        const SkinnedMesh::SpringBone& s = mesh.springBones[i];
        std::printf("  [%zu] %s bone=%d parent=%d radius=%.3f stiffness=%.2f drag=%.2f\n", i,
                    s.name.c_str(), s.boneIndex, s.parentBoneIndex, s.colliderRadius, s.stiffness,
                    s.dragForce);
    }
    if (mesh.springBones.size() > 8) {
        std::printf("  ... 其余 %zu 个省略\n", mesh.springBones.size() - 8);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: vrm_dump <模型路径>\n");
        return 2;
    }
    DumpDocument(argv[1]);
    DumpMesh(argv[1]);
    return 0;
}
