// PPet - C++ 移植版
// 仅供无头单元测试使用的渲染层桩实现（stub）。
//
// 背景：仓库中只有 src/render/Renderer.hpp（接口契约），没有对应的 .cpp 实现。
// 本文件提供一份「不含任何 OpenGL 调用」的最小实现，让 PMX 模块的单元测试
// 可以在没有 GL 上下文的环境下链接并运行。
//
// 注意：一旦 src/render/Renderer.cpp 落地（主协调者提供），本文件就不再参与
// 编译（CMake 中按文件是否存在决定），避免重复定义。
#include <src/tools/render/Renderer.hpp>

#include <utility>

namespace ppet {

// --- Shader ---------------------------------------------------------------
Shader::~Shader() = default;

bool Shader::Compile(const std::string&, const std::string&, std::string& error) {
    error = "测试桩：不支持编译着色器";
    return false;
}

void Shader::Use() const {}
void Shader::SetMat4(const char*, const Mat4&) const {}
void Shader::SetVec3(const char*, const Vec3&) const {}
void Shader::SetVec4(const char*, float, float, float, float) const {}
void Shader::SetFloat(const char*, float) const {}
void Shader::SetInt(const char*, int) const {}

// --- Texture --------------------------------------------------------------
Texture::~Texture() = default;

bool Texture::Create(const unsigned char*, int, int, int, bool, bool) { return false; }

bool Texture::LoadFromFile(const std::string&, bool, bool) { return false; }

void Texture::Bind(unsigned int) const {}

// --- SkinnedMeshRenderer --------------------------------------------------
SkinnedMeshRenderer::~SkinnedMeshRenderer() = default;

bool SkinnedMeshRenderer::Upload(const SkinnedMesh&) { return false; }

void SkinnedMeshRenderer::SetTextures(std::vector<Texture> textures) {
    textures_ = std::move(textures);
}

void SkinnedMeshRenderer::Draw(const Mat4&, const std::vector<Mat4>&, const Vec3&) {}

void SkinnedMeshRenderer::DrawOutline(const Mat4&, const std::vector<Mat4>&, const Vec3&,
                                      float) {}

}  // namespace ppet
