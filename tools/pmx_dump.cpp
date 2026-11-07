// PPet - C++ 移植版
// 用真实模型样本验证解析 + 转换的正确性（可作为回归基准）。
//
// 用法：pmx_dump <模型路径>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <src/tools/model/SkinnedMesh.hpp>
#include <src/tools/model/pmx/PmxModel.hpp>
#include <src/tools/model/pmx/PmxParser.hpp>

using namespace ppet;
using namespace ppet::pmx;

namespace {

void DumpDocument(const std::string& path) {
    PmxDocument doc;
    std::string error;
    if (!ParsePmx(path, doc, error)) {
        std::printf("解析失败: %s\n", error.c_str());
        return;
    }
    std::printf("=== 文档 (PMX %.1f, %s) ===\n", doc.version, doc.utf16 ? "UTF-16LE" : "UTF-8");
    std::printf("name=%s | nameEn=%s\n", doc.name.c_str(), doc.nameEn.c_str());
    std::printf("vertices=%zu faces=%zu dropped=%zu\n", doc.vertices.size(), doc.faces.size(),
                static_cast<std::size_t>(doc.droppedFaces));
    std::printf("textures=%zu materials=%zu bones=%zu morphs=%zu frames=%zu rb=%zu joints=%zu\n",
                doc.textures.size(), doc.materials.size(), doc.bones.size(), doc.morphs.size(),
                doc.displayFrames.size(), doc.rigidBodies.size(), doc.joints.size());

    std::uint32_t faceSum = 0;
    for (const PmxMaterial& m : doc.materials) faceSum += m.faceCount;
    std::printf("材质 faceCount 之和 = %u, 总索引数 = %zu\n", faceSum, doc.faces.size() * 3);
    std::printf("纹理列表:\n");
    for (std::size_t i = 0; i < doc.textures.size(); ++i) {
        std::printf("  [%zu] %s\n", i, doc.textures[i].c_str());
    }
    std::printf("材质:\n");
    for (std::size_t i = 0; i < doc.materials.size(); ++i) {
        const PmxMaterial& m = doc.materials[i];
        std::printf("  [%zu] %-18s faces=%-6u tex=%d sphere=%d toonFlag=%u toon=%d flags=0x%02X\n", i,
                    m.name.c_str(), m.faceCount, m.textureIndex, m.sphereTextureIndex, m.toonFlag,
                    m.toonIndex, m.flags);
    }
}

void DumpMesh(const std::string& path) {
    PmxModel model;
    std::string error;
    if (!model.Load(path, error)) {
        std::printf("加载失败: %s\n", error.c_str());
        return;
    }
    const SkinnedMesh& mesh = model.Mesh();
    std::printf("=== 转换后的 SkinnedMesh ===\n");
    std::printf("vertices=%zu indices=%zu materials=%zu bones=%zu subMeshes=%zu morphs=%zu\n",
                mesh.vertices.size(), mesh.indices.size(), mesh.materials.size(), mesh.bones.size(),
                mesh.subMeshes.size(), mesh.morphs.size());
    std::printf("bounds=(%.3f %.3f %.3f)..(%.3f %.3f %.3f) height=%.3f\n", mesh.boundsMin.x,
                mesh.boundsMin.y, mesh.boundsMin.z, mesh.boundsMax.x, mesh.boundsMax.y,
                mesh.boundsMax.z, mesh.modelHeight);
    std::printf("head=%d leftEye=%d rightEye=%d\n", mesh.headBoneIndex, mesh.leftEyeBoneIndex,
                mesh.rightEyeBoneIndex);

    // 拓扑校验：父骨骼必须排在子骨骼之前
    int bad = 0;
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].parent >= 0 &&
            static_cast<std::size_t>(mesh.bones[i].parent) >= i) {
            ++bad;
        }
    }
    std::printf("拓扑错误(父下标>=自身)的骨骼数 = %d\n", bad);

    // 索引合法性
    std::size_t badIndex = 0;
    for (std::uint32_t idx : mesh.indices) {
        if (idx >= mesh.vertices.size()) ++badIndex;
    }
    std::printf("越界索引数 = %zu\n", badIndex);

    // 权重合法性
    std::size_t badWeight = 0;
    for (const Vertex& v : mesh.vertices) {
        float sum = 0.0f;
        for (float w : v.weights) sum += w;
        if (std::fabs(sum - 1.0f) > 1e-3f) ++badWeight;
    }
    std::printf("权重和不为 1 的顶点数 = %zu\n", badWeight);

    // 子网格覆盖检查
    std::size_t covered = 0;
    for (const SkinnedMesh::SubMesh& s : mesh.subMeshes) covered += s.indexCount;
    std::printf("子网格覆盖索引 = %zu / 总索引 = %zu\n", covered, mesh.indices.size());

    // 骨骼世界矩阵与蒙皮矩阵
    std::vector<Mat4> skin;
    ComputeSkinningMatrices(const_cast<SkinnedMesh&>(mesh), skin);
    std::printf("蒙皮矩阵数 = %zu\n", skin.size());

    // 头部骨骼信息
    if (mesh.headBoneIndex >= 0) {
        const Bone& head = mesh.bones[static_cast<std::size_t>(mesh.headBoneIndex)];
        std::printf("头部骨骼: %s (parent=%d)\n", head.name.c_str(), head.parent);
    }
    if (mesh.leftEyeBoneIndex >= 0) {
        std::printf("左眼骨骼: %s\n", mesh.bones[static_cast<std::size_t>(mesh.leftEyeBoneIndex)].name.c_str());
    }
    if (mesh.rightEyeBoneIndex >= 0) {
        std::printf("右眼骨骼: %s\n", mesh.bones[static_cast<std::size_t>(mesh.rightEyeBoneIndex)].name.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: pmx_dump <模型路径>\n");
        return 2;
    }
    DumpDocument(argv[1]);
    DumpMesh(argv[1]);
    return 0;
}
