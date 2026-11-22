// PPet - C++ 移植版
// morph 法线归一化与权重钳制单测（报告 3.4.11）。
// SkinnedMesh.cpp 只依赖 header-only 的 Math.hpp，直接编入本目标，
// 无需链接 ppet_model（避免引入 GL/Qt 等重量级依赖）。
#include <src/tools/model/SkinnedMesh.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

bool Nearly(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

using namespace ppet;

float Norm(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// 构造一个含 1 个 morph 的最小网格：
///   - 顶点 0：基准法线 (0,1,0)，morph 法线偏移 (1,0,0)
///   - 顶点 1：基准法线 (0,1,0)，morph 法线偏移 (0,0,0)（不参与）
SkinnedMesh MakeMesh() {
    SkinnedMesh mesh;
    Vertex v0;
    v0.position = {0.0f, 0.0f, 0.0f};
    v0.normal = {0.0f, 1.0f, 0.0f};
    Vertex v1;
    v1.position = {1.0f, 0.0f, 0.0f};
    v1.normal = {0.0f, 1.0f, 0.0f};
    mesh.vertices = {v0, v1};

    SkinnedMesh::MorphTarget mt;
    mt.name = "smile";
    mt.positions = {{0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    mt.normals = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    mesh.morphs = {mt};
    return mesh;
}

void TestWeightClamp() {
    std::printf("-- 权重钳制到 [0,1] --\n");
    SkinnedMesh mesh = MakeMesh();
    std::vector<Vertex> out;

    // 权重 2.0 应被钳到 1.0：位移 0.5 * 1 = 0.5，而非 1.0。
    ApplyMorphs(mesh, {2.0f}, out);
    Check(Nearly(out[0].position.y, 0.5f), "权重 >1 钳到 1（位移 0.5）");

    // 权重 -1.0 应被钳到 0：无位移。
    ApplyMorphs(mesh, {-1.0f}, out);
    Check(Nearly(out[0].position.y, 0.0f), "权重 <0 钳到 0（无反向变形）");

    // 正常权重 0.5：位移 0.25。
    ApplyMorphs(mesh, {0.5f}, out);
    Check(Nearly(out[0].position.y, 0.25f), "合法权重 0.5 保持");

    // 非有限权重（NaN / inf）应被忽略，不污染顶点。
    ApplyMorphs(mesh, {std::numeric_limits<float>::quiet_NaN()}, out);
    Check(Nearly(out[0].position.y, 0.0f), "NaN 权重被忽略");
    ApplyMorphs(mesh, {std::numeric_limits<float>::infinity()}, out);
    Check(Nearly(out[0].position.y, 0.0f), "inf 权重被忽略");
}

void TestNormalNormalization() {
    std::printf("-- 法线叠加后归一化 --\n");
    SkinnedMesh mesh = MakeMesh();
    std::vector<Vertex> out;

    // 权重 1：基准法线 (0,1,0) + 偏移 (1,0,0) = (1,1,0)，归一化后长度应为 1。
    ApplyMorphs(mesh, {1.0f}, out);
    const float n = Norm(out[0].normal);
    Check(Nearly(n, 1.0f), "叠加后法线长度为 1");
    Check(Nearly(out[0].normal.x, 0.70710678f) && Nearly(out[0].normal.y, 0.70710678f),
          "归一化方向正确 (1,1,0)/√2");

    // 未参与 morph 的顶点（法线偏移为 0）保持单位长度。
    Check(Nearly(Norm(out[1].normal), 1.0f), "未参与顶点法线仍为单位长度");

    // 权重 0：法线不变，仍是单位向量。
    ApplyMorphs(mesh, {0.0f}, out);
    Check(Nearly(Norm(out[0].normal), 1.0f) && Nearly(out[0].normal.y, 1.0f),
          "权重 0 时法线保持 (0,1,0)");
}

void TestMultipleMorphs() {
    std::printf("-- 多 morph 叠加 --\n");
    SkinnedMesh mesh = MakeMesh();
    SkinnedMesh::MorphTarget mt2;
    mt2.name = "angry";
    mt2.positions = {{0.0f, 0.0f, 0.3f}, {0.0f, 0.0f, 0.0f}};
    mt2.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    mesh.morphs.push_back(mt2);

    std::vector<Vertex> out;
    // 两个 morph 各 1.0：位移 (0,0.5,0.3)；法线 (0,1,0)+(1,0,0)+(0,0,1)=(1,1,1)。
    ApplyMorphs(mesh, {1.0f, 1.0f}, out);
    Check(Nearly(out[0].position.y, 0.5f) && Nearly(out[0].position.z, 0.3f),
          "多 morph 位移正确叠加");
    Check(Nearly(Norm(out[0].normal), 1.0f), "多 morph 叠加后法线仍归一");
    Check(Nearly(out[0].normal.x, 0.57735027f) &&
              Nearly(out[0].normal.y, 0.57735027f) &&
              Nearly(out[0].normal.z, 0.57735027f),
          "多 morph 归一化方向 (1,1,1)/√3");
}

}  // namespace

int main() {
    std::printf("== morph 法线归一化与权重钳制单测 ==\n");
    TestWeightClamp();
    TestNormalNormalization();
    TestMultipleMorphs();
    std::printf("== %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
