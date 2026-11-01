// PPet - C++ 移植版
// PMX 解析器模糊测试：随机字节 / 随机变异 / 截断等恶意构造的文件，
// 解析器必须返回错误而不是崩溃或越界访问。
//
// 用法：pmx_fuzz <合法样本路径> [迭代次数]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "model/pmx/PmxParser.hpp"

using namespace ppet::pmx;

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: pmx_fuzz <合法样本路径> [迭代次数]\n");
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
        PmxDocument doc;
        std::string error;
        if (!ParsePmx(samplePath, doc, error)) {
            std::printf("样本文件解析失败: %s\n", error.c_str());
            return 2;
        }
        std::printf("样本解析成功: %zu 顶点 / %zu 面 / %zu 骨骼\n", doc.vertices.size(),
                    doc.faces.size(), doc.bones.size());
    }

    std::mt19937 rng(20240607u);
    const std::string tmpPath = "/tmp/pmx_fuzz_case.pmx";

    int parsed = 0;
    int rejected = 0;
    for (int i = 0; i < iterations; ++i) {
        std::vector<unsigned char> data;

        switch (i % 4) {
            case 0: {
                // 纯随机字节（保留 "PMX " 魔数，让解析器走到更深处）
                data.resize(1 + rng() % 4096);
                for (unsigned char& b : data) b = static_cast<unsigned char>(rng() & 0xFF);
                if (data.size() >= 4) std::memcpy(data.data(), "PMX ", 4);
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

        PmxDocument doc;
        std::string error;
        const bool ok = ParsePmx(tmpPath, doc, error);
        if (ok) {
            ++parsed;
            // 解析成功时也不允许出现越界索引。
            for (const auto& v : doc.vertices) {
                for (int k = 0; k < 4; ++k) {
                    if (v.boneIndices[k] >= static_cast<int>(doc.bones.size()) &&
                        v.boneIndices[k] != -1) {
                        std::printf("迭代 %d: 顶点骨骼索引越界 %d\n", i, v.boneIndices[k]);
                        return 1;
                    }
                }
            }
            for (const auto& m : doc.materials) {
                if (m.textureIndex >= static_cast<int>(doc.textures.size()) && m.textureIndex != -1) {
                    std::printf("迭代 %d: 材质贴图索引越界 %d\n", i, m.textureIndex);
                    return 1;
                }
            }
        } else {
            ++rejected;
            if (error.empty()) {
                std::printf("迭代 %d: 失败时错误信息为空\n", i);
                return 1;
            }
        }
    }

    std::printf("模糊测试完成: %d 次迭代，成功解析 %d 次，安全拒绝 %d 次\n", iterations, parsed,
                rejected);
    return 0;
}
