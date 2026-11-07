// PPet - C++ 移植版
// 渲染层冒烟测试。
//
// 覆盖范围：
//   1. OpenGL 上下文：GLFW 隐藏窗口 + GLEW 初始化，确认拿到 GL 3.3 core 上下文
//   2. Shader：正常源码能编译链接；错误源码必须返回 false 且 error 里有中文说明与原始日志
//   3. Texture：Create / LoadFromFile / 文件缺失回退 / 移动语义（移动后原对象 id 归零）
//   4. SkinnedMeshRenderer：立方体渲染 + glReadPixels 确认真的画出了东西
//   5. 子网格材质：同一网格两个子网格用两种材质，确认颜色分块正确（验证逐子网格 draw call）
//   6. 顶点变换正确性：已知位置的三角形必须出现在期望的屏幕位置
//      （验证列主序、Y 轴方向、透视除法这几处最容易搞反的地方）
//   7. 蒙皮矩阵：骨骼矩阵平移后顶点必须跟着移动（验证 UBO 的 std140 布局）
//   8. 纹理朝向：4 象限 UV + 已知像素的贴图，渲染结果必须与「模型 UV 原点在左上」一致
//   9. 描边：描边 pass 必须在本体轮廓之外画出颜色
//  10. Camera：自动取景的相机必须位于模型 +Z 一侧
//
// 运行方式：
//   xvfb-run -a ./ppet_render_smoke
//   或先 Xvfb :99 & 再 DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./ppet_render_smoke
//
// 退出码：0 = 全部通过；1 = 存在失败项；2 = 无图形显示环境，已跳过图形相关检查。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/SkinnedMesh.hpp>
#include <src/tools/render/Camera.hpp>
#include <src/tools/render/RenderUtils.hpp>
#include <src/tools/render/Renderer.hpp>

namespace {

using namespace ppet;

constexpr int kWindowWidth = 256;
constexpr int kWindowHeight = 256;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (condition) {
        std::printf("  [ OK ] %s\n", description.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", description.c_str());
    }
}

/// 读取单个像素，返回 0..1 的 RGBA。
Vec4 ReadPixel(int x, int y) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return Vec4(px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, px[3] / 255.0f);
}

/// 统计整帧里「不透明」（alpha > 0.5）的像素数量。
int CountOpaquePixels() {
    std::vector<unsigned char> buffer(static_cast<std::size_t>(kWindowWidth) * kWindowHeight * 4);
    glReadPixels(0, 0, kWindowWidth, kWindowHeight, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    int count = 0;
    for (std::size_t i = 3; i < buffer.size(); i += 4) {
        if (buffer[i] > 127) ++count;
    }
    return count;
}

/// 统计整帧里「非全透明」的像素数量（alpha > 0）。
int CountNonTransparentPixels() {
    std::vector<unsigned char> buffer(static_cast<std::size_t>(kWindowWidth) * kWindowHeight * 4);
    glReadPixels(0, 0, kWindowWidth, kWindowHeight, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    int count = 0;
    for (std::size_t i = 3; i < buffer.size(); i += 4) {
        if (buffer[i] > 0) ++count;
    }
    return count;
}

bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

/// 颜色比较：按通道判断，每通道容差 tolerance（0..1）。
bool ColorNear(const Vec4& actual, float r, float g, float b, float tolerance) {
    return Near(actual.x, r, tolerance) && Near(actual.y, g, tolerance) &&
           Near(actual.z, b, tolerance);
}

/// 颜色断言：失败时把实际读到的数值打印出来，方便定位。
void CheckColor(const Vec4& actual, float r, float g, float b, float tolerance,
                const std::string& description) {
    if (!ColorNear(actual, r, g, b, tolerance)) {
        std::printf("         实际读到 (%.3f, %.3f, %.3f, %.3f)，期望 (%.3f, %.3f, %.3f)\n",
                    actual.x, actual.y, actual.z, actual.w, r, g, b);
    }
    Check(ColorNear(actual, r, g, b, tolerance), description);
}

/// 清屏（透明 + 深度），并把混合 / 深度测试设成渲染层期望的初始状态。
void BeginFrame() {
    glViewport(0, 0, kWindowWidth, kWindowHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// ---------------------------------------------------------------------------
// 测试用网格构造
// ---------------------------------------------------------------------------

/// 单位立方体：24 顶点（每面独立，法线正确）+ 36 索引。
SkinnedMesh BuildCube(float halfSize) {
    SkinnedMesh mesh;
    struct FaceDef {
        Vec3 normal;
        Vec3 right;
        Vec3 up;
    };
    // 六个面：法线 + 面内的两个切向轴。
    const FaceDef faces[6] = {
        {Vec3(0, 0, 1), Vec3(1, 0, 0), Vec3(0, 1, 0)},   // +Z
        {Vec3(0, 0, -1), Vec3(-1, 0, 0), Vec3(0, 1, 0)}, // -Z
        {Vec3(1, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0)},  // +X
        {Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0)},  // -X
        {Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, 0, -1)},  // +Y
        {Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1)},  // -Y
    };
    for (const FaceDef& face : faces) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        const Vec3 center = face.normal * halfSize;
        for (int i = 0; i < 4; ++i) {
            // 面内四个角：(-1,-1) (1,-1) (1,1) (-1,1)
            const float sx = (i == 0 || i == 3) ? -1.0f : 1.0f;
            const float sy = (i < 2) ? -1.0f : 1.0f;
            Vertex v;
            v.position = center + face.right * (sx * halfSize) + face.up * (sy * halfSize);
            v.normal = face.normal;
            v.uv = Vec2((sx + 1.0f) * 0.5f, (sy + 1.0f) * 0.5f);
            v.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
            mesh.vertices.push_back(v);
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    }
    SkinnedMesh::SubMesh sub;
    sub.indexOffset = 0;
    sub.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    sub.materialIndex = 0;
    mesh.subMeshes.push_back(sub);
    return mesh;
}

/// 半径 radius 的 UV 球，法线平滑（用来说明描边：只有平滑法线在轮廓处才朝侧面）。
SkinnedMesh BuildSphere(float radius, int segments, int rings) {
    SkinnedMesh mesh;
    for (int i = 0; i <= rings; ++i) {
        const float theta = static_cast<float>(kPi) * static_cast<float>(i) /
                           static_cast<float>(rings);
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int j = 0; j <= segments; ++j) {
            const float phi = 2.0f * static_cast<float>(kPi) * static_cast<float>(j) /
                              static_cast<float>(segments);
            const Vec3 n(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            Vertex v;
            v.position = n * radius;
            v.normal = n;
            v.uv = Vec2(static_cast<float>(j) / static_cast<float>(segments),
                        static_cast<float>(i) / static_cast<float>(rings));
            v.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
            mesh.vertices.push_back(v);
        }
    }
    const auto rowStride = static_cast<std::uint32_t>(segments + 1);
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            const std::uint32_t a = static_cast<std::uint32_t>(i) * rowStride +
                                    static_cast<std::uint32_t>(j);
            const std::uint32_t b = a + rowStride;
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(a + 1);
            mesh.indices.push_back(a + 1);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b + 1);
        }
    }
    SkinnedMesh::SubMesh sub;
    sub.indexOffset = 0;
    sub.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    sub.materialIndex = 0;
    mesh.subMeshes.push_back(sub);
    return mesh;
}

/// 轴对齐矩形（z = 0），四个角按「左上、右上、右下、左下」排列，UV 与之对应。
SkinnedMesh BuildQuad(float x0, float y0, float x1, float y1) {
    SkinnedMesh mesh;
    const Vec3 positions[4] = {
        Vec3(x0, y1, 0.0f), Vec3(x1, y1, 0.0f), Vec3(x1, y0, 0.0f), Vec3(x0, y0, 0.0f)};
    const Vec2 uvs[4] = {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = positions[i];
        v.normal = Vec3(0.0f, 0.0f, 1.0f);
        v.uv = uvs[i];
        v.color = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

/// 一片材质（unlit + 纯色），便于用像素颜色反推绘制是否正确。
Material MakeUnlitMaterial(float r, float g, float b) {
    Material m;
    m.diffuse = Vec4(r, g, b, 1.0f);
    m.unlit = true;
    m.useToon = false;
    m.doubleSided = true;
    return m;
}

/// 构造「固定映射」的相机：世界坐标 [-1,1] 的 XY 直接对应 NDC [-1,1]。
/// 这样像素位置可以手算，是验证矩阵约定的关键。
Camera MakeFixedCamera(float eyeZ = 2.0f) {
    Camera cam;
    cam.projection = Ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
    cam.LookAt(Vec3(0.0f, 0.0f, eyeZ), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    return cam;
}

// ---------------------------------------------------------------------------
// 各测试项
// ---------------------------------------------------------------------------

void TestShader() {
    std::printf("[测试] Shader 编译与错误报告\n");

    const char* goodVert = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
void main() { gl_Position = vec4(aPosition, 1.0); }
)";
    const char* goodFrag = R"(#version 330 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    const char* badFrag = R"(#version 330 core
out vec4 fragColor;
void main() { this is not valid glsl }
)";

    Shader shader;
    std::string error;
    const bool ok = shader.Compile(goodVert, goodFrag, error);
    Check(ok, "合法的着色器源码编译链接成功");
    Check(error.empty(), "编译成功时 error 为空");
    Check(shader.Id() != 0, "编译成功时 program id 非零");

    // 失败路径：必须返回 false，并把中文说明 + 原始日志写进 error。
    Shader badShader;
    std::string badError;
    const bool bad = badShader.Compile(goodVert, badFrag, badError);
    Check(!bad, "非法着色器源码编译失败");
    Check(!badError.empty(), "编译失败时 error 非空");
    Check(badError.find("失败") != std::string::npos, "错误信息包含中文说明");
    Check(badError.find("ERROR") != std::string::npos,
          "错误信息包含驱动返回的原始 GLSL 日志");
    Check(badShader.Id() == 0, "编译失败时 program id 为零");

    // uniform 设置不应产生 GL 错误（名字不存在时应当被静默忽略）。
    shader.Use();
    shader.SetMat4("uMissing", Mat4::Identity());
    shader.SetVec3("uMissing", Vec3(1, 0, 0));
    shader.SetVec4("uMissing", 1, 0, 0, 0);
    shader.SetFloat("uMissing", 1.0f);
    shader.SetInt("uMissing", 1);
    Check(glGetError() == GL_NO_ERROR, "设置不存在的 uniform 不产生 GL 错误");
}

void TestTexture() {
    std::printf("[测试] Texture 创建、加载与移动语义\n");

    // 直接创建 2x2 RGBA。
    const unsigned char pixels[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    Texture texture;
    const bool created = texture.Create(pixels, 2, 2, 4, false, false);
    Check(created, "Texture::Create 成功");
    Check(texture.IsValid(), "创建后的纹理 IsValid 为真");
    Check(texture.Id() != 0, "创建后的纹理 id 非零");

    // 文件不存在必须返回 false 而不是抛异常。
    Texture missing;
    const bool missingOk = missing.LoadFromFile("tools/testdata/__不存在的文件__.png");
    Check(!missingOk, "加载不存在的文件返回 false");
    Check(!missing.IsValid(), "加载失败后纹理保持无效");

    // 真实 PNG 文件。
    Texture loaded;
    const bool loadedOk = loaded.LoadFromFile("tools/testdata/test_texture.png");
    Check(loadedOk, "LoadFromFile 加载 test_texture.png 成功");
    Check(loaded.IsValid(), "加载后的纹理 IsValid 为真");

    // 不能只看返回值：还要确认解码出来的内容与 PNG 里的真实像素一致。
    // test_texture.png 是 4x4 RGB（colortype=2，无 alpha），内容为：
    //   红绿蓝白 / 绿蓝白红 / 蓝白红绿 / 白红绿蓝（按图像行序）
    // 把纹理画到全屏四边形上，比对四个角的颜色，
    // 就把「stb 解码 -> 垂直翻转 -> 上传 -> 采样」整条链路验证了。
    //
    // 采样点取画面极角（像素 1 / 254）：那里对应贴图的角落 texel，
    // 双线性过滤在 CLAMP_TO_EDGE 下取到的是纯 texel 颜色，不会掺进邻接 texel。
    {
        SkinnedMesh mesh = BuildQuad(-1.0f, -1.0f, 1.0f, 1.0f);
        Material material = MakeUnlitMaterial(1.0f, 1.0f, 1.0f);
        material.baseColorTex = 0;
        mesh.materials.push_back(material);

        SkinnedMeshRenderer renderer;
        renderer.Upload(mesh);
        std::vector<Texture> textures;
        textures.push_back(std::move(loaded));
        renderer.SetTextures(std::move(textures));

        const Camera cam = MakeFixedCamera();
        BeginFrame();
        renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));

        // 屏幕左上角对应模型 UV(0,0) = 图像左上 = 像素(0,0) = 红；
        // 屏幕右上角对应图像(0,3) = 白；
        // 屏幕左下角对应图像(3,0) = 白；
        // 屏幕右下角对应图像(3,3) = 蓝。
        const Vec4 topLeft = ReadPixel(1, kWindowHeight - 2);
        const Vec4 topRight = ReadPixel(kWindowWidth - 2, kWindowHeight - 2);
        const Vec4 bottomLeft = ReadPixel(1, 1);
        const Vec4 bottomRight = ReadPixel(kWindowWidth - 2, 1);
        CheckColor(topLeft, 1.0f, 0.0f, 0.0f, 0.02f, "PNG 左上角 = 红（解码+翻转正确）");
        CheckColor(topRight, 1.0f, 1.0f, 1.0f, 0.02f, "PNG 右上角 = 白");
        CheckColor(bottomLeft, 1.0f, 1.0f, 1.0f, 0.02f, "PNG 左下角 = 白");
        CheckColor(bottomRight, 0.0f, 0.0f, 1.0f, 0.02f, "PNG 右下角 = 蓝");
    }

    // 移动语义：移动后源对象 id 必须归零，否则析构时会 double free。
    const unsigned int originalId = loaded.Id();
    Texture moved(std::move(loaded));
    Check(moved.Id() == originalId, "移动后目标对象持有原 id");
    Check(loaded.Id() == 0, "移动后源对象 id 归零（避免 double free）");
    Check(!loaded.IsValid(), "移动后源对象 IsValid 为假");

    // 移动赋值同样要清零源对象。
    Texture assigned;
    assigned = std::move(moved);
    Check(assigned.Id() == originalId, "移动赋值后目标对象持有原 id");
    Check(moved.Id() == 0, "移动赋值后源对象 id 归零");
}

/// 立方体渲染：确认真的画出了东西，且颜色正确。
void TestRenderCube() {
    std::printf("[测试] SkinnedMeshRenderer 渲染立方体\n");

    SkinnedMesh mesh = BuildCube(0.6f);
    Material material = MakeUnlitMaterial(1.0f, 0.0f, 0.0f);
    mesh.materials.push_back(material);

    SkinnedMeshRenderer renderer;
    Check(renderer.Upload(mesh), "Upload 立方体成功");

    const Camera cam = MakeFixedCamera();
    const std::vector<Mat4> noSkinning;

    // 画若干帧，确认不会随帧数累积出 GL 错误。
    for (int frame = 0; frame < 5; ++frame) {
        BeginFrame();
        renderer.Draw(cam.viewProjection, noSkinning, Vec3(0.0f, 0.0f, 1.0f));
    }
    Check(glGetError() == GL_NO_ERROR, "连续绘制若干帧后没有 GL 错误");

    const int opaque = CountOpaquePixels();
    Check(opaque > 0, "帧缓冲里存在不透明像素（真的画出了东西）");
    std::printf("         不透明像素数 = %d / %d\n", opaque, kWindowWidth * kWindowHeight);

    // 立方体半边长 0.6，相机正交范围 [-1,1]，
    // 因此中心区域应当是材质色（红色），角落应当是背景。
    const Vec4 center = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(ColorNear(center, 1.0f, 0.0f, 0.0f, 0.02f), "中心像素是材质色（红色）");
    const Vec4 corner = ReadPixel(2, 2);
    Check(corner.w < 0.02f, "角落像素是透明背景");
}

/// 两个子网格两种材质：验证逐子网格 draw call 与材质切换。
void TestSubMeshMaterials() {
    std::printf("[测试] 子网格材质切换\n");

    SkinnedMesh mesh;
    // 左半边一个矩形，右半边一个矩形，中间留缝。
    const SkinnedMesh left = BuildQuad(-0.9f, -0.4f, -0.1f, 0.4f);
    const SkinnedMesh right = BuildQuad(0.1f, -0.4f, 0.9f, 0.4f);

    const std::uint32_t vertexBase = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), left.vertices.begin(), left.vertices.end());
    mesh.vertices.insert(mesh.vertices.end(), right.vertices.begin(), right.vertices.end());
    for (std::uint32_t index : left.indices) {
        mesh.indices.push_back(vertexBase + index);
    }
    const std::uint32_t rightBase = vertexBase + static_cast<std::uint32_t>(left.vertices.size());
    for (std::uint32_t index : right.indices) {
        mesh.indices.push_back(rightBase + index);
    }

    mesh.materials.push_back(MakeUnlitMaterial(1.0f, 0.0f, 0.0f)); // 材质 0：红
    mesh.materials.push_back(MakeUnlitMaterial(0.0f, 0.0f, 1.0f)); // 材质 1：蓝
    SkinnedMesh::SubMesh subLeft;
    subLeft.indexOffset = 0;
    subLeft.indexCount = static_cast<std::uint32_t>(left.indices.size());
    subLeft.materialIndex = 0;
    SkinnedMesh::SubMesh subRight;
    subRight.indexOffset = static_cast<std::uint32_t>(left.indices.size());
    subRight.indexCount = static_cast<std::uint32_t>(right.indices.size());
    subRight.materialIndex = 1;
    mesh.subMeshes.push_back(subLeft);
    mesh.subMeshes.push_back(subRight);

    SkinnedMeshRenderer renderer;
    Check(renderer.Upload(mesh), "Upload 双子网格网格成功");

    const Camera cam = MakeFixedCamera();
    BeginFrame();
    renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));

    const Vec4 leftPixel = ReadPixel(64, kWindowHeight / 2);
    const Vec4 rightPixel = ReadPixel(kWindowWidth - 64, kWindowHeight / 2);
    Check(ColorNear(leftPixel, 1.0f, 0.0f, 0.0f, 0.02f), "左半边是材质 0 的红色");
    Check(ColorNear(rightPixel, 0.0f, 0.0f, 1.0f, 0.02f), "右半边是材质 1 的蓝色");
}

/// 顶点变换正确性：这是最容易出错的地方（列主序 / Y 轴 / 透视除法）。
void TestVertexTransform() {
    std::printf("[测试] 顶点变换正确性（屏幕位置）\n");

    // 三角形覆盖 NDC 的右上角区域：
    //   世界坐标 (0,0) (1,0) (0,1) -> NDC 同样（正交相机 [-1,1] 且不做任何翻转）。
    // glReadPixels 的 y=0 是屏幕底部，NDC y=+1 是屏幕顶部，
    // 因此这块三角形应该出现在「右上角」。
    SkinnedMesh mesh = BuildQuad(0.0f, 0.0f, 1.0f, 1.0f);
    mesh.materials.push_back(MakeUnlitMaterial(0.0f, 1.0f, 0.0f));

    SkinnedMeshRenderer renderer;
    Check(renderer.Upload(mesh), "Upload 测试三角形成功");

    const Camera cam = MakeFixedCamera();
    BeginFrame();
    renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));

    // NDC (0.5, 0.5) -> 像素 (0.75*W, 0.75*H)
    const int expectX = static_cast<int>(0.75f * static_cast<float>(kWindowWidth));
    const int expectY = static_cast<int>(0.75f * static_cast<float>(kWindowHeight));
    const Vec4 inside = ReadPixel(expectX, expectY);
    Check(ColorNear(inside, 0.0f, 1.0f, 0.0f, 0.02f), "三角形内部在期望的屏幕位置（右上）");

    // 左下角必须没有像素（若 Y 轴被翻转，这里会变成有像素）。
    const Vec4 opposite = ReadPixel(64, 64);
    Check(opposite.w < 0.02f, "对角位置（左下）没有像素，说明 Y 轴方向正确");

    // 左上角必须没有像素（若 X 轴被翻转，这里会变成有像素）。
    const Vec4 mirroredX = ReadPixel(64, kWindowHeight - 64);
    Check(mirroredX.w < 0.02f, "镜像位置（左上）没有像素，说明 X 轴方向正确");

    // 透视相机下同样要落在屏幕中心附近（验证透视除法与 w 分量处理正确）。
    SkinnedMesh tri;
    {
        // 一个位于原点的三角形：透视下应当出现在屏幕中心。
        Vertex v;
        v.position = Vec3(-0.2f, -0.2f, 0.0f);
        v.normal = Vec3(0.0f, 0.0f, 1.0f);
        tri.vertices.push_back(v);
        v.position = Vec3(0.2f, -0.2f, 0.0f);
        tri.vertices.push_back(v);
        v.position = Vec3(0.0f, 0.2f, 0.0f);
        tri.vertices.push_back(v);
        tri.indices = {0, 1, 2};
        tri.materials.push_back(MakeUnlitMaterial(1.0f, 1.0f, 0.0f));
    }
    SkinnedMeshRenderer perspRenderer;
    perspRenderer.Upload(tri);
    Camera perspCam;
    perspCam.projection = Perspective(30.0f * kPi / 180.0f, 1.0f, 0.1f, 10.0f);
    perspCam.LookAt(Vec3(0.0f, 0.0f, 2.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    BeginFrame();
    perspRenderer.Draw(perspCam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));
    const Vec4 perspCenter = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(ColorNear(perspCenter, 1.0f, 1.0f, 0.0f, 0.02f), "透视相机下三角形出现在屏幕中心");
}

/// 蒙皮矩阵：验证 UBO 的 std140 布局与蒙皮约定。
void TestSkinning() {
    std::printf("[测试] 骨骼蒙皮矩阵\n");

    SkinnedMesh mesh = BuildQuad(-0.1f, -0.1f, 0.1f, 0.1f);
    mesh.materials.push_back(MakeUnlitMaterial(1.0f, 1.0f, 1.0f));
    // 一根骨骼，权重全在它身上。
    Bone bone;
    bone.name = "root";
    mesh.bones.push_back(bone);

    SkinnedMeshRenderer renderer;
    Check(renderer.Upload(mesh), "Upload 带骨骼的网格成功");

    const Camera cam = MakeFixedCamera();

    // 未施加蒙皮时，矩形在屏幕中心。
    BeginFrame();
    renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));
    const Vec4 beforeMove = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(beforeMove.w > 0.5f, "未施加蒙皮时矩形在屏幕中心");

    // 骨骼矩阵平移 (0.5, 0, 0)。
    // 正交相机范围是 [-1,1]，画面 256 像素，所以 1 个世界单位 = 128 像素，
    // 0.5 个世界单位 = 64 像素。矩形中心原本在屏幕中心 (128,128)，
    // 平移后应落在 (192,128)。
    std::vector<Mat4> skinning;
    skinning.push_back(Mat4::Translation(Vec3(0.5f, 0.0f, 0.0f)));
    BeginFrame();
    renderer.Draw(cam.viewProjection, skinning, Vec3(0.0f, 0.0f, 1.0f));

    const Vec4 oldCenter = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(oldCenter.w < 0.02f, "施加骨骼平移后原位置变空");
    const Vec4 newCenter = ReadPixel(kWindowWidth / 2 + 64, kWindowHeight / 2);
    Check(newCenter.w > 0.5f, "顶点跟随骨骼矩阵平移了 64 像素");
}

/// 纹理朝向：模型 UV 原点在左上，贴图内部已翻转，两者必须抵消。
void TestTextureOrientation() {
    std::printf("[测试] 纹理朝向与 sRGB 往返\n");

    // 用 8x8 的「纯色象限」贴图而不是 2x2：
    // 2x2 贴图的每个 texel 覆盖半个屏幕，采样点必然落在 texel 边界附近，
    // 线性过滤会掺进邻接 texel 的颜色（例如红里混进一点绿），
    // 那样测的就不是朝向而是过滤精度了。
    // 8x8 每个象限 4x4 同色，采样点在象限内任意位置都是纯色。
    constexpr int kSize = 8;
    constexpr int kHalf = kSize / 2;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    // 输入的第 0 行是「图像上方」：
    //   左上=红 右上=绿   <- 图像上行
    //   左下=蓝 右下=白   <- 图像下行
    const unsigned char quadrant[4][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int qx = (x < kHalf) ? 0 : 1;
            const int qy = (y < kHalf) ? 0 : 1;
            const unsigned char* color = quadrant[qy * 2 + qx];
            unsigned char* px = pixels.data() + (static_cast<std::size_t>(y) * kSize + x) * 4;
            px[0] = color[0];
            px[1] = color[1];
            px[2] = color[2];
            px[3] = 255;
        }
    }

    // 覆盖整屏的矩形，UV 左上角 = (0,0)。
    SkinnedMesh mesh = BuildQuad(-1.0f, -1.0f, 1.0f, 1.0f);
    Material material = MakeUnlitMaterial(1.0f, 1.0f, 1.0f);
    material.baseColorTex = 0;
    mesh.materials.push_back(material);

    SkinnedMeshRenderer renderer;
    renderer.Upload(mesh);

    Texture texture;
    texture.Create(pixels.data(), kSize, kSize, 4, /*srgb=*/true, /*mipmap=*/false);

    std::vector<Texture> textures;
    textures.push_back(std::move(texture));
    renderer.SetTextures(std::move(textures));

    const Camera cam = MakeFixedCamera();
    BeginFrame();
    renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));

    // 采样点取各象限纹理中心对应的屏幕位置（避免落在纹理边缘）。
    const int half = kWindowWidth / 2;
    const Vec4 topLeft = ReadPixel(half / 2, kWindowHeight - half / 2);
    const Vec4 topRight = ReadPixel(kWindowWidth - half / 2, kWindowHeight - half / 2);
    const Vec4 bottomLeft = ReadPixel(half / 2, half / 2);
    const Vec4 bottomRight = ReadPixel(kWindowWidth - half / 2, half / 2);

    CheckColor(topLeft, 1.0f, 0.0f, 0.0f, 0.02f, "屏幕左上 = 贴图左上（红）");
    CheckColor(topRight, 0.0f, 1.0f, 0.0f, 0.02f, "屏幕右上 = 贴图右上（绿）");
    CheckColor(bottomLeft, 0.0f, 0.0f, 1.0f, 0.02f, "屏幕左下 = 贴图左下（蓝）");
    CheckColor(bottomRight, 1.0f, 1.0f, 1.0f, 0.02f, "屏幕右下 = 贴图右下（白）");
}

/// 描边：必须在本体轮廓之外画出一圈。
void TestOutline() {
    std::printf("[测试] 描边 pass\n");

    SkinnedMesh mesh = BuildSphere(0.5f, 24, 12);
    Material material = MakeUnlitMaterial(1.0f, 1.0f, 1.0f);
    material.outlineWidth = 0.05f;
    material.outlineColor = Vec3(0.0f, 0.0f, 1.0f);
    mesh.materials.push_back(material);

    SkinnedMeshRenderer renderer;
    Check(renderer.Upload(mesh), "Upload 球体成功");

    const Camera cam = MakeFixedCamera();

    // 先画本体。
    BeginFrame();
    renderer.Draw(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));
    const int bodyPixels = CountOpaquePixels();
    // 球半径 0.5 -> NDC 0.5 -> 像素半径 64，球心在屏幕中心 (128,128)，
    // 因此轮廓最右端在 x = 192。读 192 恰好落在球面切线上（像素中心在球外），
    // 所以往里取一点，读轮廓内侧的像素。
    const Vec4 bodyEdge = ReadPixel(static_cast<int>(0.70f * kWindowWidth), kWindowHeight / 2);
    Check(bodyEdge.w > 0.5f, "本体轮廓内侧是不透明像素");
    const Vec4 outsideBody = ReadPixel(kWindowWidth - 12, kWindowHeight / 2);
    Check(outsideBody.w < 0.02f, "本体轮廓之外是透明的");

    // 再画描边。
    renderer.DrawOutline(cam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f), 1.0f);
    Check(glGetError() == GL_NO_ERROR, "描边 pass 没有 GL 错误");

    const int outlinePixels = CountNonTransparentPixels();
    Check(outlinePixels > bodyPixels, "描边 pass 在本体之外增加了像素");
    std::printf("         本体像素 = %d，描边后 = %d\n", bodyPixels, outlinePixels);

    // 轮廓外侧的一圈应当是描边色（蓝色）。
    // 球半径 0.5 -> NDC 0.5 -> 像素 192；描边外扩 0.05 -> NDC 0.55 -> 像素约 198。
    const Vec4 outlinePixel = ReadPixel(196, kWindowHeight / 2);
    Check(ColorNear(outlinePixel, 0.0f, 0.0f, 1.0f, 0.05f), "本体轮廓之外一圈是描边色（蓝）");
}

/// 相机自动取景：必须在模型 +Z 一侧（模型正面朝 +Z）。
void TestCamera() {
    std::printf("[测试] Camera 自动取景\n");

    SkinnedMesh mesh = BuildCube(0.5f);
    mesh.boundsMin = Vec3(-0.5f, -0.5f, -0.5f);
    mesh.boundsMax = Vec3(0.5f, 0.5f, 0.5f);
    mesh.modelHeight = 1.0f;

    const Vec3 center = (mesh.boundsMin + mesh.boundsMax) * 0.5f;

    const Camera ortho = Camera::FitOrthographic(mesh, 1.0f);
    Check(ortho.position.z > 0.0f, "正交相机位于模型 +Z 一侧（看到正面）");
    Check(Near(ortho.target.x, center.x, 1e-4f) && Near(ortho.target.y, center.y, 1e-4f) &&
              Near(ortho.target.z, center.z, 1e-4f),
          "正交相机看向模型中心");

    const Camera persp = Camera::FitPerspective(mesh, 1.0f);
    Check(persp.position.z > 0.0f, "透视相机位于模型 +Z 一侧（看到正面）");
    Check(Near(persp.target.x, center.x, 1e-4f) && Near(persp.target.y, center.y, 1e-4f) &&
              Near(persp.target.z, center.z, 1e-4f),
          "透视相机看向模型中心");

    // viewProjection 必须把模型中心投到 NDC 原点附近。
    const Vec3 ndc = ortho.viewProjection.TransformPoint(center);
    Check(std::fabs(ndc.x) < 0.05f && std::fabs(ndc.y) < 0.05f,
          "正交相机把模型中心投到屏幕中心");
    const Vec3 perspNdc = persp.viewProjection.TransformPoint(center);
    Check(std::fabs(perspNdc.x) < 0.05f && std::fabs(perspNdc.y) < 0.05f,
          "透视相机把模型中心投到屏幕中心");
}

/// toon 光照：验证「受光面 = 材质色，阴影面 = 阴影色 × 材质色」这条公式。
void TestToonShading() {
    std::printf("[测试] toon 光照公式\n");

    // shadeColor 默认 (0.3,0.3,0.3)，diffuse 用白色 (1,1,1)，
    // 于是受光面应为 1.0，阴影面应为 0.3。
    Material material;
    material.diffuse = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    material.shadeColor = Vec3(0.3f, 0.3f, 0.3f);
    material.useToon = true;
    material.unlit = false;
    material.doubleSided = true;
    material.lightIntensity = 1.0f;
    material.ambient = Vec3(0.0f, 0.0f, 0.0f);
    material.emissive = Vec3(0.0f, 0.0f, 0.0f);

    // 受光面：法线朝 +Z，与光源同向 -> N·L = 1 -> shade = 1。
    SkinnedMesh lit = BuildQuad(-0.9f, -0.9f, -0.1f, 0.9f);
    lit.materials.push_back(material);
    // 阴影面：法线朝 -Z，与光源相反 -> N·L = -1 -> shade = 0。
    SkinnedMesh shadow = BuildQuad(0.1f, -0.9f, 0.9f, 0.9f);
    for (Vertex& v : shadow.vertices) v.normal = Vec3(0.0f, 0.0f, -1.0f);
    shadow.materials.push_back(material);

    SkinnedMeshRenderer litRenderer;
    SkinnedMeshRenderer shadowRenderer;
    litRenderer.Upload(lit);
    shadowRenderer.Upload(shadow);

    const Camera cam = MakeFixedCamera();
    // 光源在 +Z 方向。
    const Vec3 lightDir(0.0f, 0.0f, 1.0f);

    BeginFrame();
    litRenderer.Draw(cam.viewProjection, std::vector<Mat4>(), lightDir);
    shadowRenderer.Draw(cam.viewProjection, std::vector<Mat4>(), lightDir);

    const Vec4 litPixel = ReadPixel(64, kWindowHeight / 2);
    const Vec4 shadowPixel = ReadPixel(kWindowWidth - 64, kWindowHeight / 2);
    CheckColor(litPixel, 1.0f, 1.0f, 1.0f, 0.03f, "受光面 = 材质色（白）");
    CheckColor(shadowPixel, 0.3f, 0.3f, 0.3f, 0.03f, "阴影面 = 阴影色 × 材质色（0.3 灰）");
}

/// sRGB <-> 线性空间的 CPU 侧往返。
void TestSrgbRoundTrip() {
    std::printf("[测试] sRGB 往返\n");

    const Vec3 probes[4] = {Vec3(0.0f, 0.5f, 1.0f), Vec3(0.25f, 0.75f, 0.1f),
                            Vec3(0.04f, 0.5f, 0.9f), Vec3(1.0f, 1.0f, 1.0f)};
    for (const Vec3& c : probes) {
        const Vec3 back = LinearToSrgb(SrgbToLinear(c));
        Check(Near(back.x, c.x, 1e-4f) && Near(back.y, c.y, 1e-4f) && Near(back.z, c.z, 1e-4f),
              "sRGB -> 线性 -> sRGB 往返无损");
    }
    // 中灰的线性值必须明显小于 sRGB 值（否则说明转换没生效）。
    const Vec3 mid = SrgbToLinear(Vec3(0.5f, 0.5f, 0.5f));
    Check(mid.x < 0.25f, "sRGB 中灰转到线性空间后变暗");
}

/// 边缘光：需要「从表面指向相机」的方向 V。
/// 渲染层只拿到 VP 矩阵，V 必须从矩阵里反解，
/// 所以这里用「球体边缘有边缘光、中心没有」来验证反解是否正确。
void TestRimLight() {
    std::printf("[测试] 边缘光（视线方向反解）\n");

    Material material;
    material.diffuse = Vec4(0.0f, 0.0f, 0.0f, 1.0f);  // 基础色全黑，只留边缘光
    material.rimColor = Vec3(0.0f, 0.0f, 1.0f);
    material.rimLightingMix = 1.0f;
    material.rimFresnelPower = 1.0f;
    material.useToon = true;
    material.unlit = false;
    material.doubleSided = true;
    material.lightIntensity = 1.0f;
    material.ambient = Vec3(0.0f, 0.0f, 0.0f);
    material.emissive = Vec3(0.0f, 0.0f, 0.0f);
    material.shadeColor = Vec3(0.0f, 0.0f, 0.0f);

    SkinnedMesh mesh = BuildSphere(0.6f, 32, 16);
    mesh.materials.push_back(material);

    SkinnedMeshRenderer renderer;
    renderer.Upload(mesh);

    // 透视相机：视线方向必须逐顶点变化（边缘更掠射）。
    Camera perspCam;
    perspCam.projection = Perspective(30.0f * kPi / 180.0f, 1.0f, 0.1f, 10.0f);
    perspCam.LookAt(Vec3(0.0f, 0.0f, 3.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    BeginFrame();
    renderer.Draw(perspCam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));

    // 球心（正对相机）几乎没有边缘光，球体轮廓处最亮。
    const Vec4 center = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(center.z < 0.1f, "透视：球心（正对相机）边缘光很弱");

    // 轮廓处：找一列像素里最亮的那个，它应该明显比球心亮。
    float edgeMax = 0.0f;
    for (int x = 0; x < kWindowWidth; ++x) {
        const Vec4 p = ReadPixel(x, kWindowHeight / 2);
        edgeMax = std::max(edgeMax, p.z);
    }
    Check(edgeMax > 0.3f, "透视：球体轮廓处有明显的边缘光");
    std::printf("         轮廓处边缘光强度 = %.3f\n", edgeMax);

    // 正交相机：视线方向是常量，边缘光分布应当同样只出现在轮廓处。
    const Camera orthoCam = MakeFixedCamera(2.0f);
    BeginFrame();
    renderer.Draw(orthoCam.viewProjection, std::vector<Mat4>(), Vec3(0.0f, 0.0f, 1.0f));
    const Vec4 orthoCenter = ReadPixel(kWindowWidth / 2, kWindowHeight / 2);
    Check(orthoCenter.z < 0.1f, "正交：球心（正对相机）边缘光很弱");
    float orthoEdgeMax = 0.0f;
    for (int x = 0; x < kWindowWidth; ++x) {
        const Vec4 p = ReadPixel(x, kWindowHeight / 2);
        orthoEdgeMax = std::max(orthoEdgeMax, p.z);
    }
    Check(orthoEdgeMax > 0.3f, "正交：球体轮廓处有明显的边缘光");
    std::printf("         轮廓处边缘光强度 = %.3f\n", orthoEdgeMax);
}

}  // namespace

int main() {
    std::printf("PPet 渲染层冒烟测试\n");
    std::printf("--------------------------------------------------------------\n");

    if (!glfwInit()) {
        std::printf("glfwInit 失败：当前环境没有可用的图形显示，跳过图形相关检查。\n");
        return 2;
    }

    // 隐藏窗口：测试不需要真的显示出来。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(kWindowWidth, kWindowHeight, "PPet Render Smoke", nullptr, nullptr);
    if (window == nullptr) {
        std::printf("创建 GLFW 窗口失败：无法获取 GL 3.3 core 上下文，跳过图形相关检查。\n");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);

    // GLEW 在 core profile 下必须先打开 glewExperimental，否则会报「无效操作」。
    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK) {
        std::printf("glewInit 失败（%d）：%s\n", static_cast<int>(glewStatus),
                    reinterpret_cast<const char*>(glewGetErrorString(glewStatus)));
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
    }
    // glewInit 之后要把遗留的错误清掉，否则后续的 GL 错误检查会误报。
    while (glGetError() != GL_NO_ERROR) {
    }

    std::printf("GL_VERSION  = %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("GLSL        = %s\n",
                reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
    std::printf("Renderer    = %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    std::printf("--------------------------------------------------------------\n");

    // 帧缓冲必须完整，否则读回像素没有意义。
    const GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    Check(fbStatus == GL_FRAMEBUFFER_COMPLETE, "默认帧缓冲完整（可以读回像素）");
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
    }

    TestShader();
    TestTexture();
    TestRenderCube();
    TestSubMeshMaterials();
    TestVertexTransform();
    TestSkinning();
    TestTextureOrientation();
    TestToonShading();
    TestRimLight();
    TestSrgbRoundTrip();
    TestOutline();
    TestCamera();

    std::printf("--------------------------------------------------------------\n");
    std::printf("检查项：%d，失败：%d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("全部通过。\n");
    } else {
        std::printf("存在失败项。\n");
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return g_failures == 0 ? 0 : 1;
}
