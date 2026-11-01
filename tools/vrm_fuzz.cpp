// PPet - C++ 移植版
// VRM 解析器模糊测试：随机字节 / 随机变异 / 截断等恶意构造的文件，
// 解析器必须返回错误而不是崩溃或越界访问。
//
// 用法：vrm_fuzz <合法样本路径> [迭代次数]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "model/SkinnedMesh.hpp"
#include "model/vrm/GltfParser.hpp"
#include "model/vrm/VrmModel.hpp"

using namespace ppet;
using namespace ppet::vrm;

namespace {

bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return read == out.size();
}

bool WriteFile(const std::string& path, const std::vector<unsigned char>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const std::size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return written == data.size();
}

/// 检查转换结果里的所有索引都在合法范围内。
///
/// 解析成功不代表转换成功：转换层还要保证不会用越界的下标去访问数组。
bool CheckMeshIndices(const SkinnedMesh& mesh, int iteration) {
    const int boneCount = static_cast<int>(mesh.bones.size());
    const int morphCount = static_cast<int>(mesh.morphs.size());
    const int materialCount = static_cast<int>(mesh.materials.size());

    for (const auto& sub : mesh.subMeshes) {
        if (sub.materialIndex < 0 || sub.materialIndex >= materialCount) {
            std::printf("迭代 %d: 子网格材质索引越界 %d\n", iteration, sub.materialIndex);
            return false;
        }
        // indexOffset / indexCount 是无符号的，只需检查上界。
        if (static_cast<std::size_t>(sub.indexOffset) > mesh.indices.size()) {
            std::printf("迭代 %d: 子网格索引起点越界 %u\n", iteration, sub.indexOffset);
            return false;
        }
        if (static_cast<std::size_t>(sub.indexOffset) + sub.indexCount > mesh.indices.size()) {
            std::printf("迭代 %d: 子网格索引数量越界 %u\n", iteration, sub.indexCount);
            return false;
        }
    }
    for (const auto& expression : mesh.expressions) {
        for (const auto& binding : expression.bindings) {
            if (binding.first < 0 || binding.first >= morphCount) {
                std::printf("迭代 %d: 表情绑定的 morph 索引越界 %d\n", iteration, binding.first);
                return false;
            }
        }
    }
    for (const auto& spring : mesh.springBones) {
        if (spring.boneIndex < 0 || spring.boneIndex >= boneCount) {
            std::printf("迭代 %d: 弹簧骨骨骼索引越界 %d\n", iteration, spring.boneIndex);
            return false;
        }
        if (spring.parentBoneIndex >= boneCount) {
            std::printf("迭代 %d: 弹簧骨父骨骼索引越界 %d\n", iteration, spring.parentBoneIndex);
            return false;
        }
    }
    if (mesh.headBoneIndex >= boneCount || mesh.leftEyeBoneIndex >= boneCount ||
        mesh.rightEyeBoneIndex >= boneCount) {
        std::printf("迭代 %d: 视线控制骨骼索引越界\n", iteration);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: vrm_fuzz <合法样本路径> [迭代次数]\n");
        return 2;
    }
    const std::string samplePath = argv[1];
    const int iterations = (argc >= 3) ? std::atoi(argv[2]) : 2000;

    std::vector<unsigned char> sample;
    if (!ReadFile(samplePath, sample)) {
        std::printf("无法读取样本文件: %s\n", samplePath.c_str());
        return 2;
    }

    // 基准：样本本身必须能解析成功，否则说明样本有问题。
    {
        GltfDocument doc;
        std::string error;
        if (!LoadGltf(samplePath, doc, error)) {
            std::printf("样本文件解析失败: %s\n", error.c_str());
            return 2;
        }
        std::printf("样本解析成功: %zu 节点 / %zu 网格 / %zu 骨骼\n", doc.Nodes().size(),
                    doc.Meshes().size(), doc.Skins().size());
    }

    std::mt19937 rng(20240607u);
    const std::string tmpPath = "/tmp/vrm_fuzz_case.vrm";

    int parsed = 0;
    int rejected = 0;
    for (int i = 0; i < iterations; ++i) {
        std::vector<unsigned char> data;

        switch (i % 4) {
            case 0: {
                // 纯随机字节（保留 "glTF" 魔数，让解析器走到更深处）
                data.resize(1 + rng() % 4096);
                for (unsigned char& b : data) b = static_cast<unsigned char>(rng() & 0xFF);
                if (data.size() >= 4) std::memcpy(data.data(), "glTF", 4);
                break;
            }
            case 1: {
                // 变异合法样本：随机翻转若干字节
                data = sample;
                const std::size_t flips = 1 + rng() % 32;
                for (std::size_t k = 0; k < flips && !data.empty(); ++k) {
                    data[rng() % data.size()] = static_cast<unsigned char>(rng() & 0xFF);
                }
                break;
            }
            case 2: {
                // 截断合法样本
                const std::size_t len = rng() % (sample.size() + 1);
                data.assign(sample.begin(), sample.begin() + static_cast<long>(len));
                break;
            }
            default: {
                // 随机插入垃圾字节
                data = sample;
                const std::size_t inserts = 1 + rng() % 16;
                for (std::size_t k = 0; k < inserts; ++k) {
                    const std::size_t pos = rng() % (data.size() + 1);
                    data.insert(data.begin() + static_cast<long>(pos),
                                static_cast<unsigned char>(rng() & 0xFF));
                }
                break;
            }
        }

        if (!WriteFile(tmpPath, data)) {
            std::printf("无法写入临时文件\n");
            return 2;
        }

        GltfDocument doc;
        std::string error;
        if (!LoadGltf(tmpPath, doc, error)) {
            ++rejected;
            if (error.empty()) {
                std::printf("迭代 %d: 失败时错误信息为空\n", i);
                return 1;
            }
            continue;
        }
        ++parsed;

        // 转换层同样不能崩，也不能产出越界的索引。
        VrmModel model;
        std::string loadError;
        if (model.Load(tmpPath, loadError)) {
            if (!CheckMeshIndices(model.Mesh(), i)) return 1;
        } else if (loadError.empty()) {
            std::printf("迭代 %d: 转换失败时错误信息为空\n", i);
            return 1;
        }
    }

    std::printf("模糊测试完成: %d 次迭代，成功解析 %d 次，安全拒绝 %d 次\n", iterations, parsed,
                rejected);
    return 0;
}
