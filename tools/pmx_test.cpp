// PPet - C++ 移植版
// PMX 解析器 / 转换层单元测试。
//
// 覆盖内容：
//   1. 用 tools/gen_test_pmx.py 生成的测试模型验证各字段解析结果。
//   2. 验证 UTF-16LE 与 UTF-8 两种文本编码分支。
//   3. 验证 PMX -> SkinnedMesh 转换（坐标系、骨骼拓扑排序、权重、子网格、材质）。
//   4. 健壮性：截断文件、随机字节文件、伪造头文件必须返回错误而不是崩溃。
//
// 不依赖任何测试框架，用简单的 CHECK 宏实现断言。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <src/tools/model/SkinnedMesh.hpp>
#include <src/tools/model/pmx/PmxModel.hpp>
#include <src/tools/model/pmx/PmxParser.hpp>

using namespace ppet;
using namespace ppet::pmx;

namespace {

int g_failures = 0;
int g_checks = 0;

void Report(bool ok, const char* expr, const char* file, int line, const std::string& extra) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  [FAIL] %s:%d: %s", file, line, expr);
    if (!extra.empty()) std::printf("  (%s)", extra.c_str());
    std::printf("\n");
}

#define CHECK(cond) Report((cond), #cond, __FILE__, __LINE__, std::string())
#define CHECK_MSG(cond, msg) Report((cond), #cond, __FILE__, __LINE__, (msg))

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

std::string TestDataDir() {
    // 默认从可执行文件所在目录的 ../tools/testdata 找；
    // 也允许用环境变量 PPET_TESTDATA 覆盖，方便在 CI 中指定路径。
    const char* env = std::getenv("PPET_TESTDATA");
    if (env != nullptr && env[0] != '\0') return std::string(env);
    return "tools/testdata";
}

// ---------------------------------------------------------------------------
// 1. 解析结果断言
// ---------------------------------------------------------------------------

void TestParseFields() {
    std::printf("[测试] 解析字段正确性 (test.pmx)\n");
    PmxDocument doc;
    std::string error;
    const std::string path = TestDataDir() + "/test.pmx";
    const bool ok = ParsePmx(path, doc, error);
    CHECK_MSG(ok, error);
    if (!ok) return;

    CHECK(Near(doc.version, 2.0f));
    CHECK(doc.utf16);                       // 本测试文件用 UTF-16LE
    CHECK(doc.name == "テストモデル");
    CHECK(doc.nameEn == "Test Model");
    CHECK(doc.comment == "PMX 解析器测试用模型");
    CHECK(doc.commentEn == "Model used by the PMX parser test suite");

    CHECK(doc.vertices.size() == 24);
    CHECK(doc.faces.size() == 12);
    CHECK(doc.textures.size() == 1);
    CHECK(doc.textures[0] == "test_texture.png");
    CHECK(doc.materials.size() == 2);
    CHECK(doc.bones.size() == 6);
    CHECK(doc.morphs.size() == 2);
    CHECK(doc.displayFrames.size() == 2);
    CHECK(doc.rigidBodies.size() == 1);
    CHECK(doc.joints.size() == 1);
    CHECK(doc.droppedFaces == 0);

    // 顶点：位置 / 法线 / UV / 附加 UV / 权重
    // 立方体 6 个面各 4 个顶点：面 0（前面）中心 (0,1,-1)，法线 -Z。
    const PmxVertex& v0 = doc.vertices[0];
    CHECK(Near(v0.position[0], -1.0f) && Near(v0.position[1], 2.0f) && Near(v0.position[2], -1.0f));
    CHECK(Near(v0.normal[0], 0.0f) && Near(v0.normal[1], 0.0f) && Near(v0.normal[2], -1.0f));
    CHECK(Near(v0.uv[0], 0.0f) && Near(v0.uv[1], 0.0f));
    CHECK(v0.additionalUvs.size() == 1);

    // 权重类型：按顶点下标轮转覆盖 BDEF1 / BDEF2 / BDEF4 / SDEF
    for (std::size_t i = 0; i < doc.vertices.size(); ++i) {
        const PmxWeightType expect =
            (i % 4 == 0) ? PmxWeightType::BDef1
            : (i % 4 == 1) ? PmxWeightType::BDef2
            : (i % 4 == 2) ? PmxWeightType::BDef4
                           : PmxWeightType::SDef;
        CHECK_MSG(doc.vertices[i].weightType == expect, "顶点权重类型应按 i%4 轮转");
    }

    CHECK(doc.vertices[0].boneIndices[0] == 0);
    CHECK(Near(doc.vertices[0].weights[0], 1.0f));
    CHECK(doc.vertices[1].boneIndices[0] == 1 && doc.vertices[1].boneIndices[1] == 2);
    CHECK(Near(doc.vertices[1].weights[0], 0.7f));
    CHECK(Near(doc.vertices[1].weights[1], 0.3f));  // 解析器补出 1 - w0
    CHECK(doc.vertices[2].boneIndices[3] == 3);
    CHECK(Near(doc.vertices[2].weights[3], 0.1f));
    CHECK(doc.vertices[3].boneIndices[0] == 0 && doc.vertices[3].boneIndices[1] == 1);
    CHECK(Near(doc.vertices[3].weights[0], 0.6f));
    CHECK(Near(doc.vertices[3].weights[1], 0.4f));  // SDEF 同样补出 1 - w0
    CHECK(Near(doc.vertices[3].sdefC[0], 0.0f));
    CHECK(Near(doc.vertices[3].sdefR0[0], 1.0f));
    CHECK(Near(doc.vertices[3].sdefR1[1], 1.0f));

    // 材质
    const PmxMaterial& m0 = doc.materials[0];
    CHECK(m0.name == "マテリアル1");
    CHECK(m0.nameEn == "Material1");
    CHECK(Near(m0.diffuse[0], 0.8f) && Near(m0.diffuse[3], 1.0f));
    CHECK(Near(m0.specular[0], 0.2f));
    CHECK(Near(m0.shininess, 12.0f));
    CHECK(Near(m0.ambient[1], 0.1f));
    CHECK((m0.flags & kMaterialNoCull) != 0);
    CHECK((m0.flags & kMaterialHasEdge) != 0);
    CHECK(Near(m0.edgeSize, 1.5f));
    CHECK(m0.textureIndex == 0);
    CHECK(m0.sphereTextureIndex == -1);
    CHECK(m0.toonFlag == 0);
    CHECK(m0.toonIndex == -1);
    CHECK(m0.faceCount == 18);  // 6 个三角形 * 3
    CHECK(doc.materials[1].toonFlag == 1);
    CHECK(doc.materials[1].toonIndex == 2);

    // 骨骼：父下标大于自身的场景必须原样保留
    CHECK(doc.bones[0].name == "センター");
    CHECK(doc.bones[0].parentIndex == -1);
    CHECK(doc.bones[1].name == "頭");
    CHECK(doc.bones[1].parentIndex == 2);   // 父下标 > 自身下标
    CHECK(doc.bones[2].parentIndex == 3);
    CHECK(doc.bones[3].parentIndex == 0);
    CHECK(Near(doc.bones[3].position[1], 1.7f));
    CHECK((doc.bones[0].flags & kBoneRotatable) != 0);
    CHECK(doc.bones[0].tailIndex == 3);

    // morph：顶点形变 + UV 形变
    CHECK(doc.morphs[0].type == PmxMorphType::Vertex);
    CHECK(doc.morphs[0].elements.size() == 2);
    CHECK(doc.morphs[0].elements[0].index == 2);
    CHECK(Near(doc.morphs[0].elements[0].offset[1], 0.5f));
    CHECK(doc.morphs[1].type == PmxMorphType::Uv);
    CHECK(doc.morphs[1].elements[1].index == 1);
    CHECK(Near(doc.morphs[1].elements[1].offset[0], 0.2f));

    // 显示帧 / 刚体 / 关节
    CHECK(doc.displayFrames[0].name == "表情1");
    CHECK(doc.displayFrames[0].items.size() == 1);
    CHECK(doc.displayFrames[0].items[0].target == 1);
    CHECK(doc.displayFrames[0].items[0].index == 0);
    CHECK(doc.rigidBodies[0].name == "剛体1");
    CHECK(doc.rigidBodies[0].boneIndex == 0);
    CHECK(doc.joints[0].name == "関節1");
    CHECK(doc.joints[0].rigidBodyA == 0);
    CHECK(doc.joints[0].rigidBodyB == -1);
}

// ---------------------------------------------------------------------------
// 2. 文本编码分支
// ---------------------------------------------------------------------------

void TestEncoding() {
    std::printf("[测试] UTF-16LE / UTF-8 编码分支\n");

    PmxDocument u16;
    std::string error;
    const bool ok16 = ParsePmx(TestDataDir() + "/test_utf16.pmx", u16, error);
    CHECK_MSG(ok16, error);
    if (ok16) {
        CHECK(u16.utf16);
        CHECK(u16.name == "テストモデル");
        CHECK(u16.nameEn == "Test Model");
    }

    PmxDocument u8;
    const bool ok8 = ParsePmx(TestDataDir() + "/test_utf8.pmx", u8, error);
    CHECK_MSG(ok8, error);
    if (ok8) {
        CHECK(!u8.utf16);
        CHECK(u8.name == "テストモデル");
        CHECK(u8.nameEn == "Test Model");
        // 两种编码的模型内容必须完全一致（只是文本字节不同）。
        CHECK(u8.vertices.size() == u16.vertices.size());
        CHECK(u8.bones.size() == u16.bones.size());
        CHECK(u8.materials.size() == u16.materials.size());
    }
}

// ---------------------------------------------------------------------------
// 3. PMX -> SkinnedMesh 转换
// ---------------------------------------------------------------------------

void TestModelConversion() {
    std::printf("[测试] PMX -> SkinnedMesh 转换\n");
    PmxModel model;
    std::string error;
    const std::string path = TestDataDir() + "/test.pmx";
    const bool ok = model.Load(path, error);
    CHECK_MSG(ok, error);
    if (!ok) return;

    CHECK(model.IsLoaded());
    CHECK(model.Type() == ModelType::Pmx);
    CHECK(model.DisplayName() == "テストモデル");

    const SkinnedMesh& mesh = model.Mesh();
    CHECK(mesh.vertices.size() == 24);
    CHECK(mesh.indices.size() == 36);
    CHECK(mesh.materials.size() == 2);
    CHECK(mesh.bones.size() == 6);
    CHECK(mesh.subMeshes.size() == 2);
    CHECK(mesh.isSkinned);
    CHECK(!mesh.morphs.empty());
    CHECK(!mesh.expressions.empty());

    // 坐标系：z 取反
    CHECK(Near(mesh.vertices[0].position.z, 1.0f));
    CHECK(Near(mesh.vertices[0].normal.z, 1.0f));

    // 绕序：原三角形 (0,1,2) 应变成 (0,2,1)
    CHECK(mesh.indices[0] == 0);
    CHECK(mesh.indices[1] == 2);
    CHECK(mesh.indices[2] == 1);

    // 绕序正确性：转换到右手系后，每个三角形的叉积应与顶点法线同向
    //（即 OpenGL 需要的 CCW 正面朝外）。这是坐标变换最容易出错的地方，
    // 必须逐三角形校验而不是抽样。
    int wrongWinding = 0;
    int degenerate = 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vertex& a = mesh.vertices[mesh.indices[i]];
        const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 cr = Cross(b.position - a.position, c.position - a.position);
        const Vec3 n = a.normal + b.normal + c.normal;
        if (Dot(cr, cr) < 1e-12f) { ++degenerate; continue; }
        if (Dot(cr, n) <= 0.0f) ++wrongWinding;
    }
    CHECK_MSG(wrongWinding == 0, "存在绕序反向的三角形");
    CHECK_MSG(degenerate == 0, "存在退化三角形");

    // 子网格按材质面数切分
    CHECK(mesh.subMeshes[0].materialIndex == 0);
    CHECK(mesh.subMeshes[0].indexOffset == 0);
    CHECK(mesh.subMeshes[0].indexCount == 18);
    CHECK(mesh.subMeshes[1].materialIndex == 1);
    CHECK(mesh.subMeshes[1].indexOffset == 18);
    CHECK(mesh.subMeshes[1].indexCount == 18);

    // 骨骼拓扑排序：父必须排在子之前
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].parent >= 0) {
            CHECK(static_cast<std::size_t>(mesh.bones[i].parent) < i);
        }
    }
    // 骨骼名保留
    bool foundHead = false, foundEyeL = false, foundEyeR = false;
    for (const Bone& b : mesh.bones) {
        if (b.name == "頭") foundHead = true;
        if (b.name == "左目") foundEyeL = true;
        if (b.name == "右目") foundEyeR = true;
    }
    CHECK(foundHead);
    CHECK(foundEyeL);
    CHECK(foundEyeR);

    // 逆绑定矩阵 = 世界绑定矩阵的逆。绑定姿态下 world * inverseBind 应为单位阵。
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        Mat4 world = mesh.bones[i].localBindPose;
        int p = mesh.bones[i].parent;
        while (p >= 0) {
            world = mesh.bones[static_cast<std::size_t>(p)].localBindPose * world;
            p = mesh.bones[static_cast<std::size_t>(p)].parent;
        }
        const Mat4 skin = world * mesh.bones[i].inverseBindMatrix;
        bool identity = true;
        for (int r = 0; r < 4 && identity; ++r) {
            for (int c = 0; c < 4; ++c) {
                const float expect = (r == c) ? 1.0f : 0.0f;
                if (!Near(skin.At(r, c), expect, 1e-3f)) { identity = false; break; }
            }
        }
        CHECK_MSG(identity, mesh.bones[i].name);
    }

    // 顶点权重：BDEF2 顶点应有两个关节且权重和为 1
    const Vertex& v1 = mesh.vertices[1];
    float sum = v1.weights[0] + v1.weights[1] + v1.weights[2] + v1.weights[3];
    CHECK(Near(sum, 1.0f));
    CHECK(v1.weights[0] > 0.0f && v1.weights[1] > 0.0f);
    // SDEF 顶点（索引 3）被降级为 BDEF2，同样应是两骨骼、和为 1
    const Vertex& v3 = mesh.vertices[3];
    CHECK(Near(v3.weights[0] + v3.weights[1] + v3.weights[2] + v3.weights[3], 1.0f));
    CHECK(v3.weights[0] > 0.0f && v3.weights[1] > 0.0f);
    // 关节索引必须指向有效的骨骼
    for (const Vertex& v : mesh.vertices) {
        for (std::uint32_t j : v.joints) {
            CHECK(j < mesh.bones.size());
        }
    }
    // 所有顶点的权重和都应为 1（蒙皮的前提）
    for (const Vertex& v : mesh.vertices) {
        const float s = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
        CHECK(Near(s, 1.0f));
    }

    // 材质映射
    const Material& m0 = mesh.materials[0];
    CHECK(m0.name == "マテリアル1");
    CHECK(Near(m0.diffuse.x, 0.8f) && Near(m0.diffuse.w, 1.0f));
    CHECK(Near(m0.specular.x, 0.2f));
    CHECK(Near(m0.shininess, 12.0f));
    CHECK(Near(m0.ambient.y, 0.1f));
    CHECK(m0.doubleSided);                       // bit0 = 关闭背面剔除
    CHECK(m0.baseColorTex == 0);
    CHECK(Near(m0.edgeScale, 1.5f));              // 原始 edgeSize 保存在 edgeScale
    CHECK(m0.outlineWidth > 0.0f);               // 归一化后的世界单位宽度
    CHECK(!m0.useToon == false);                 // PMX 恒为 toon 着色
    CHECK(mesh.materials[1].toonRampTex == -1);   // 内置共享 toon 无贴图
    CHECK(mesh.materials[0].toonRampTex == -1);   // 独立 toon 索引越界也被丢弃

    // 纹理路径应带上 PMX 所在目录
    CHECK(mesh.modelHeight > 1.0f);
    CHECK(Near(mesh.boundsMin.x, -1.0f));
    CHECK(Near(mesh.boundsMax.y, 2.0f));

    // 控制骨骼识别（视线追踪的基础）
    CHECK(mesh.headBoneIndex >= 0);
    CHECK(mesh.leftEyeBoneIndex >= 0);
    CHECK(mesh.rightEyeBoneIndex >= 0);

    // 视线追踪：Update 之后头部骨骼应产生非零旋转。
    // 注意：这里必须取数值副本而不是引用——后续 Update 会改写同一根骨骼。
    const std::size_t headIdx = static_cast<std::size_t>(mesh.headBoneIndex);

    // 计算骨骼在世界空间的朝向：绑定姿态下父链没有旋转，
    // 因此世界朝向 = 骨骼自身的局部旋转作用于 +Z（模型正面）。
    auto headForward = [&mesh, headIdx]() {
        Mat4 world = mesh.bones[headIdx].currentLocal;
        int p = mesh.bones[headIdx].parent;
        while (p >= 0) {
            world = mesh.bones[static_cast<std::size_t>(p)].currentLocal * world;
            p = mesh.bones[static_cast<std::size_t>(p)].parent;
        }
        return Normalize(world.TransformDirection(Vec3(0.0f, 0.0f, 1.0f)));
    };

    RenderContext ctx;
    ctx.viewportWidth = 640;
    ctx.viewportHeight = 480;
    ctx.deltaTime = 1.0f / 60.0f;
    ctx.hasGlobalCursor = true;
    ctx.localCursor = Vec2(1.0f, 0.0f);
    for (int i = 0; i < 60; ++i) model.Update(ctx);

    const Vec3 fwdRight = headForward();
    CHECK_MSG(fwdRight.x > 0.1f, "鼠标在右方时头部应转向 +X");
    CHECK_MSG(fwdRight.z > 0.0f, "头部应仍大致朝向模型正面");

    // 鼠标移到左边，头部旋转方向应相反
    RenderContext ctx2 = ctx;
    ctx2.localCursor = Vec2(-1.0f, 0.0f);
    for (int i = 0; i < 60; ++i) model.Update(ctx2);
    const Vec3 fwdLeft = headForward();
    CHECK_MSG(fwdLeft.x < -0.1f, "鼠标在左方时头部应转向 -X");

    // 鼠标在上方时头部应抬头（世界朝向的 y 分量为正）
    RenderContext ctx3 = ctx;
    ctx3.localCursor = Vec2(0.0f, 1.0f);
    for (int i = 0; i < 60; ++i) model.Update(ctx3);
    const Vec3 fwdUp = headForward();
    CHECK_MSG(fwdUp.y > 0.05f, "鼠标在上方时头部应抬起");

    // 鼠标在下方时头部应低头
    RenderContext ctx4 = ctx;
    ctx4.localCursor = Vec2(0.0f, -1.0f);
    for (int i = 0; i < 60; ++i) model.Update(ctx4);
    const Vec3 fwdDown = headForward();
    CHECK_MSG(fwdDown.y < -0.05f, "鼠标在下方时头部应低下");
}

// ---------------------------------------------------------------------------
// 4. 健壮性
// ---------------------------------------------------------------------------

void TestRobustness() {
    std::printf("[测试] 健壮性：损坏文件必须返回错误\n");

    const char* broken[] = {
        "/broken_truncated.pmx",
        "/broken_random.pmx",
        "/broken_header_only.pmx",
        "/not_exist_file.pmx",
    };
    for (const char* name : broken) {
        PmxDocument doc;
        std::string error;
        const bool ok = ParsePmx(TestDataDir() + name, doc, error);
        CHECK_MSG(!ok, std::string("应失败却成功: ") + name);
        CHECK_MSG(!error.empty(), std::string("失败时必须给出错误信息: ") + name);
        if (!ok) {
            std::printf("    %s -> %s\n", name, error.c_str());
        }
    }

    // 健壮性同样适用于转换层：Load 失败后不能留下半成品状态。
    PmxModel model;
    std::string error;
    CHECK(!model.Load(TestDataDir() + "/broken_truncated.pmx", error));
    CHECK(!model.IsLoaded());
    CHECK(model.Mesh().vertices.empty());

    // 对空网格调用 Update / Render 不应崩溃。
    RenderContext ctx;
    ctx.viewportWidth = 320;
    ctx.viewportHeight = 240;
    ctx.deltaTime = 1.0f / 60.0f;
    model.Update(ctx);
    model.Render(ctx);
    CHECK(true);  // 未崩溃即通过
}

}  // namespace

int main() {
    std::printf("=== PPet PMX 模块测试 ===\n");
    TestParseFields();
    TestEncoding();
    TestModelConversion();
    TestRobustness();
    std::printf("=== 检查项 %d，失败 %d ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
