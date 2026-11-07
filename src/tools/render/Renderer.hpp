// PPet - C++ 移植版
// 渲染层：着色器、纹理、蒙皮网格绘制器。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/SkinnedMesh.hpp>

namespace ppet {

/// OpenGL 着色器程序封装。
class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    /// 从顶点/片元源码编译。失败时 error 中写入日志。
    bool Compile(const std::string& vertexSrc, const std::string& fragmentSrc,
                 std::string& error);

    void Use() const;
    unsigned int Id() const { return program_; }

    // 下面这组 Set* 只设置当前 program 的 uniform，
    // 调用前必须先 Use()（或由绘制流程保证 program 已激活）。
    void SetMat4(const char* name, const Mat4& m) const;
    void SetVec3(const char* name, const Vec3& v) const;
    void SetVec4(const char* name, float x, float y, float z, float w) const;
    void SetFloat(const char* name, float v) const;
    void SetInt(const char* name, int v) const;

private:
    /// 查 uniform 位置，结果缓存起来。
    /// glGetUniformLocation 每次调用都要在着色器内部做字符串比较，
    /// 每帧每个 uniform 调一次会明显拖慢渲染，所以按名字缓存。
    int UniformLocation(const char* name) const;

    unsigned int program_ = 0;
    /// uniform 名 -> location。mutable 是因为 Set* 是 const 成员函数，
    /// 但缓存本身属于「惰性求值的内部状态」，不改变对象的逻辑值。
    mutable std::unordered_map<std::string, int> uniformLocations_;
};

/// GPU 纹理。
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    /// 移动构造 / 移动赋值。
    /// 必要性：拷贝构造被 delete 后不会自动生成移动构造，而
    /// SkinnedMeshRenderer::SetTextures(std::vector<Texture>) 要求调用方能
    /// 构造并移动纹理对象（纹理加载后无法拷贝，只能移动进容器）。
    ///
    /// 注意：这里刻意不用 `= default`。默认实现只做标量拷贝，
    /// 移动之后源对象仍然持有同一个 GL 名字，源对象析构时会把纹理删掉，
    /// 目标对象随即变成野指针（double free）。
    /// 所以实现在 Renderer.cpp 中：转移 id_ 后把源对象的 id_ 置 0。
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    /// 从内存数据创建纹理（PNG/JPEG 由 stb_image 解码后传入）。
    ///
    /// 数据约定：传入的像素必须是「首行在最上面」（stb_image 的解码结果就是
    /// 这种行序）。内部会翻转成 OpenGL 的「首行在最下面」，着色器采样时用
    /// 1 - v 还原，这样 PMX 与 VRM 两个后端的画面方向一致。
    bool Create(const unsigned char* pixels, int width, int height, int channels,
                bool srgb = true, bool mipmap = true);

    /// 从文件加载纹理。找不到文件时返回 false（调用方应回退到纯色）。
    bool LoadFromFile(const std::string& path, bool srgb = true, bool mipmap = true);

    void Bind(unsigned int unit) const;
    unsigned int Id() const { return id_; }
    bool IsValid() const { return id_ != 0; }

private:
    unsigned int id_ = 0;
};

/// toon 蒙皮网格渲染器：GPU 顶点缓冲 + 骨骼矩阵上传。
class SkinnedMeshRenderer {
public:
    SkinnedMeshRenderer() = default;
    ~SkinnedMeshRenderer();

    SkinnedMeshRenderer(const SkinnedMeshRenderer&) = delete;
    SkinnedMeshRenderer& operator=(const SkinnedMeshRenderer&) = delete;

    /// 上传网格数据（顶点/索引）。纹理由调用方通过 SetTextures 提供。
    bool Upload(const SkinnedMesh& mesh);

    /// 设置材质对应的纹理列表（索引与 mesh.materials 的 texture 索引对应）。
    void SetTextures(std::vector<Texture> textures);

    /// 绘制。
    /// @param viewProjection 视图投影矩阵
    /// @param skinningMatrices 骨骼蒙皮矩阵（数量须与骨骼数一致）
    /// @param lightDir 光源方向（世界空间，指向光源）
    void Draw(const Mat4& viewProjection, const std::vector<Mat4>& skinningMatrices,
              const Vec3& lightDir);

    /// 绘制描边（法线外扩）。
    void DrawOutline(const Mat4& viewProjection, const std::vector<Mat4>& skinningMatrices,
                     const Vec3& lightDir, float widthScale);

private:
    bool UploadVertices(const SkinnedMesh& mesh);
    void DrawInternal(const Mat4& vp, const std::vector<Mat4>& skinning,
                      const Vec3& lightDir, bool outlinePass);
    /// 设置当前子网格的材质 uniform（含纹理绑定与混合状态）。
    void ApplyMaterial(const Material& material);
    /// 绑定 textures_ 中第 index 号纹理到指定单元，返回是否绑定成功。
    bool BindTexture(int index, unsigned int unit) const;

    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    unsigned int ebo_ = 0;
    unsigned int boneVbo_ = 0;
    std::size_t indexCount_ = 0;
    std::size_t boneCount_ = 0;
    /// UBO 中骨骼矩阵槽位的总数。
    /// 必须与着色器里 uBones 数组的编译期长度一致，
    /// 否则 uniform block 会越界读取缓冲区。
    std::size_t boneCapacity_ = 0;
    std::vector<Texture> textures_;
    std::shared_ptr<Shader> shader_;
    /// 描边用的独立着色器（顶点外扩 + 纯色输出）。
    std::shared_ptr<Shader> outlineShader_;
    /// 网格的材质与子网格信息。契约里 Draw() 不接收 mesh，
    /// 所以必须在 Upload() 时缓存一份。
    std::vector<Material> materials_;
    std::vector<SkinnedMesh::SubMesh> subMeshes_;
    /// 描边外扩的视距缩放系数。DrawOutline() 的 widthScale 参数
    /// 不能直接写进 uniform —— DrawInternal() 才是真正发 draw call 的地方，
    /// 中间隔了一层，所以先存在这里传递下去。
    float outlineScale_ = 1.0f;
};

/// 线性空间 / sRGB 转换辅助。
Vec3 SrgbToLinear(const Vec3& c);
Vec3 LinearToSrgb(const Vec3& c);

}  // namespace ppet
