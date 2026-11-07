// PPet - C++ 移植版
// Cubism Core 适配层：所有 csm* 调用的**唯一**集中区域。
//
// 【本文件的唯一职责】
//   把 Cubism Core 的 C 风格 API 转换成 CubismCore.hpp 里定义的 C++ 抽象接口。
//   上层（Live2DModel.cpp）不出现任何 csm* 字样，因此：
//     - 不装 SDK 也能编译上层代码；
//     - 换 SDK 版本只需要改这一个文件。
//
// 【Cubism Core 的内存模型（4.x / 5.x）】
//   Core 采用 InPlace 模型：不自己分配内存，而是由调用方准备好对齐的内存块，
//   Core 在其中「复活」结构。官方要求：
//     - moc 内存按 csmAlignofMoc（64 字节）对齐；
//     - model 内存按 csmAlignofModel（16 字节）对齐；
//     - moc 内存必须存活到所有由它生成的 model 释放之后。
//   因此本文件的 Model 里保存了 moc 内存块与 model 内存块，
//   销毁时按相反顺序释放。
//
// 【真实 SDK 与桩】
//   PPET_WITH_CUBISM      ：使用真实 Cubism SDK（third_party/Cubism/Core）。
//   PPET_WITH_CUBISM_STUB ：使用桩（tools/test_stubs/CubismStub）。
//   两者都不定义           ：本文件只提供「后端不可用」的实现，
//                           Live2DModel 会给出友好的中文提示。
//
//   桩与真实 SDK 提供**完全同名同签名**的 csm* 函数，
//   所以下面的代码在两种情况下是同一份源码，不需要条件编译。
#include <src/tools/core/FileIO.hpp>
#include <api/live2d/CubismCore.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(PPET_WITH_CUBISM)
#include "Live2DCubismCore.h"
#elif defined(PPET_WITH_CUBISM_STUB)
#include <tools/test_stubs/CubismStub/CubismStubCore.h>
#endif

namespace ppet::live2d::cubism {
namespace {

// ---------------------------------------------------------------------------
// 对齐内存分配
// ---------------------------------------------------------------------------

/// 按指定对齐分配内存。
///
/// Cubism Core 要求 moc 内存 64 字节对齐、model 内存 16 字节对齐，
/// 标准的 new/malloc 不保证这么严格的对齐，因此这里自己实现。
///
/// @param size 字节数
/// @param alignment 对齐要求（字节）
/// @return 分配到的首地址；失败返回 nullptr
void* AllocateAligned(std::size_t size, std::size_t alignment) {
    if (size == 0) size = 1;
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* raw = nullptr;
    if (posix_memalign(&raw, alignment, size) != 0) return nullptr;
    return raw;
#endif
}

/// 释放 AllocateAligned 分配的内存。
void FreeAligned(void* ptr) {
    if (!ptr) return;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

/// 读取整个文件。
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out) {
    // 走跨平台读取接口：Windows 上窄字符路径打不开含中文的模型目录。
    return ReadFileBytes(path, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Model 的实际定义
// ---------------------------------------------------------------------------

/// 模型实例。
///
/// 只在适配层内部可见，外部只持有 Model* 指针。
class Model {
public:
    Model() = default;
    ~Model() { Release(); }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    /// 释放所有 Cubism 资源。
    ///
    /// 释放顺序必须与分配顺序相反：model 内存依赖 moc 内存存活，
    /// 因此先释放 model 内存，再释放 moc 内存。
    void Release() {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
        if (modelMemory_) {
            FreeAligned(modelMemory_);
            modelMemory_ = nullptr;
        }
        model_ = nullptr;

        if (mocMemory_) {
            FreeAligned(mocMemory_);
            mocMemory_ = nullptr;
        }
        moc_ = nullptr;
        mocSize_ = 0;
#endif
    }

#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    csmMoc* moc_ = nullptr;
    csmModel* model_ = nullptr;
    void* mocMemory_ = nullptr;
    void* modelMemory_ = nullptr;
    unsigned int mocSize_ = 0;
#endif
};

namespace {

#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)

/// 绘制顺序的缓存：Core 给出的 RenderOrder 决定实际绘制次序。
///
/// Core 的 r12 文档明确指出 RenderOrder 与 DrawOrder 不同：
/// 前者是「考虑了遮罩关系之后的实际绘制顺序」。
/// 这里按 RenderOrder 升序排序，排序结果每帧都可能变化（参数变化会改变顺序）。
struct DrawableOrder {
    int index = 0;
    int renderOrder = 0;
};

/// 按 renderOrder 升序排序（稳定排序，保持同序部件的相对次序）。
[[maybe_unused]] bool CompareOrder(const DrawableOrder& a, const DrawableOrder& b) {
    return a.renderOrder < b.renderOrder;
}

#endif  // PPET_WITH_CUBISM || PPET_WITH_CUBISM_STUB

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

bool Available() {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    return true;
#else
    return false;
#endif
}

std::string Version() {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    const csmVersion v = csmGetVersion();
    // 版本号打包：Major(1) + Minor(1) + Patch(2)，共 4 字节。
    const unsigned int major = (v >> 24) & 0xFFu;
    const unsigned int minor = (v >> 16) & 0xFFu;
    const unsigned int patch = v & 0xFFFFu;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", major, minor, patch);
    return std::string(buffer);
#else
    return std::string();
#endif
}

Model* CreateModel() {
    return new (std::nothrow) Model();
}

void DestroyModel(Model* model) {
    delete model;
}

bool OpenModel(Model* model, const std::string& mocPath, std::string& error) {
    if (!model) {
        error = "模型实例为空";
        return false;
    }

#if !defined(PPET_WITH_CUBISM) && !defined(PPET_WITH_CUBISM_STUB)
    error = "Live2D 模型需要编译 Cubism SDK 后端（CMake 选项 PPET_WITH_CUBISM）";
    return false;
#else
    // 已有模型时先释放旧资源，避免内存泄漏。
    model->Release();

    std::vector<unsigned char> fileData;
    if (!ReadWholeFile(mocPath, fileData)) {
        error = "无法读取 moc 文件：" + mocPath;
        return false;
    }

    // --- 准备 moc 内存（64 字节对齐）---
    const std::size_t mocBytes = fileData.size();
    void* mocMemory = AllocateAligned(mocBytes, 64);
    if (!mocMemory) {
        error = "内存不足，无法为 moc 文件分配对齐内存";
        return false;
    }
    std::memcpy(mocMemory, fileData.data(), mocBytes);

    // --- 复活 moc ---
    csmMoc* moc = csmReviveMocInPlace(mocMemory, static_cast<unsigned int>(mocBytes));
    if (!moc) {
        FreeAligned(mocMemory);
        error = "moc 文件解析失败（文件可能损坏或不是有效的 Cubism 模型）：" + mocPath;
        return false;
    }

    // --- 准备 model 内存（16 字节对齐）---
    const unsigned int modelBytes = csmGetSizeofModel(moc);
    if (modelBytes == 0) {
        FreeAligned(mocMemory);
        error = "无法确定模型结构大小，moc 文件可能不受支持：" + mocPath;
        return false;
    }
    void* modelMemory = AllocateAligned(modelBytes, 16);
    if (!modelMemory) {
        FreeAligned(mocMemory);
        error = "内存不足，无法为模型结构分配对齐内存";
        return false;
    }

    // --- 初始化 model ---
    csmModel* csm = csmInitializeModelInPlace(moc, modelMemory, modelBytes);
    if (!csm) {
        FreeAligned(modelMemory);
        FreeAligned(mocMemory);
        error = "模型初始化失败：" + mocPath;
        return false;
    }

    model->moc_ = moc;
    model->model_ = csm;
    model->mocMemory_ = mocMemory;
    model->modelMemory_ = modelMemory;
    model->mocSize_ = static_cast<unsigned int>(mocBytes);
    return true;
#endif
}

void UpdateModel(Model* model) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return;
    // 调用顺序与官方 Cubism Framework 的 CubismModel::Update() 一致：
    // 先更新顶点（Core 在此刻把「与上一帧的差异」写进动态标志），
    // 再复位动态标志，为下一帧的差异检测做准备。
    csmUpdateModel(model->model_);
    csmResetDrawableDynamicFlags(model->model_);
#else
    (void)model;
#endif
}

bool ReadCanvasInfo(const Model* model, CanvasInfo& out) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return false;

    csmVector2 size;
    csmVector2 origin;
    float pixelsPerUnit = 1.0f;
    csmReadCanvasInfo(model->model_, &size, &origin, &pixelsPerUnit);

    out.sizeInPixels = Vec2{size.X, size.Y};
    out.originInPixels = Vec2{origin.X, origin.Y};
    out.pixelsPerUnit = (pixelsPerUnit > 0.0f) ? pixelsPerUnit : 1.0f;
    return true;
#else
    (void)model;
    (void)out;
    return false;
#endif
}

int ParameterCount(const Model* model) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return 0;
    return csmGetParameterCount(model->model_);
#else
    (void)model;
    return 0;
#endif
}

std::string ParameterId(const Model* model, int index) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return std::string();
    const int count = csmGetParameterCount(model->model_);
    if (index < 0 || index >= count) return std::string();
    const char** ids = csmGetParameterIds(model->model_);
    if (!ids || !ids[index]) return std::string();
    return std::string(ids[index]);
#else
    (void)model;
    (void)index;
    return std::string();
#endif
}

int FindParameter(const Model* model, const std::string& id) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_ || id.empty()) return -1;
    const int count = csmGetParameterCount(model->model_);
    const char** ids = csmGetParameterIds(model->model_);
    if (!ids) return -1;
    for (int i = 0; i < count; ++i) {
        if (ids[i] && id == ids[i]) return i;
    }
    return -1;
#else
    (void)model;
    (void)id;
    return -1;
#endif
}

float ParameterValue(const Model* model, int index) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return 0.0f;
    float* values = csmGetParameterValues(model->model_);
    if (!values) return 0.0f;
    const int count = csmGetParameterCount(model->model_);
    if (index < 0 || index >= count) return 0.0f;
    return values[index];
#else
    (void)model;
    (void)index;
    return 0.0f;
#endif
}

void SetParameterValue(Model* model, int index, float value) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return;
    float* values = csmGetParameterValues(model->model_);
    if (!values) return;
    const int count = csmGetParameterCount(model->model_);
    if (index < 0 || index >= count) return;
    values[index] = value;
#else
    (void)model;
    (void)index;
    (void)value;
#endif
}

bool ParameterRange(const Model* model, int index, float& outMin, float& outMax) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return false;
    const int count = csmGetParameterCount(model->model_);
    if (index < 0 || index >= count) return false;
    const float* mins = csmGetParameterMinimumValues(model->model_);
    const float* maxs = csmGetParameterMaximumValues(model->model_);
    if (!mins || !maxs) return false;
    outMin = mins[index];
    outMax = maxs[index];
    return true;
#else
    (void)model;
    (void)index;
    (void)outMin;
    (void)outMax;
    return false;
#endif
}

int DrawableCount(const Model* model) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return 0;
    return csmGetDrawableCount(model->model_);
#else
    (void)model;
    return 0;
#endif
}

bool GetDrawable(const Model* model, int index, Drawable& out) {
#if defined(PPET_WITH_CUBISM) || defined(PPET_WITH_CUBISM_STUB)
    if (!model || !model->model_) return false;
    const int count = csmGetDrawableCount(model->model_);
    if (index < 0 || index >= count) return false;

    // --- 顶点数与索引数 ---
    const int* vertexCounts = csmGetDrawableVertexCounts(model->model_);
    const int* indexCounts = csmGetDrawableIndexCounts(model->model_);
    if (!vertexCounts || !indexCounts) return false;

    const int vertexCount = vertexCounts[index];
    const int indexCount = indexCounts[index];
    if (vertexCount <= 0 || indexCount <= 0) return false;

    // --- 顶点与 UV（锯齿数组）---
    //
    // Core 的顶点/UV/索引都是「锯齿数组」：第 d 项指向第 d 个部件自己的数组。
    // 索引空间按部件独立，因此不能把多个部件的索引拼进同一个缓冲。
    const csmVector2** positions = csmGetDrawableVertexPositions(model->model_);
    const csmVector2** uvs = csmGetDrawableVertexUvs(model->model_);
    const unsigned short** indices = csmGetDrawableIndices(model->model_);
    if (!positions || !uvs || !indices) return false;
    if (!positions[index] || !uvs[index] || !indices[index]) return false;

    // Core 用 csmVector2（{float X, float Y}）存储坐标，
    // 本抽象层用交错的 float 数组，二者内存布局完全一致。
    out.positions = reinterpret_cast<const float*>(positions[index]);
    out.uvs = reinterpret_cast<const float*>(uvs[index]);
    out.indices = indices[index];
    out.vertexCount = vertexCount;
    out.indexCount = indexCount;

    // --- 纹理序号 ---
    const int* textureIndices = csmGetDrawableTextureIndices(model->model_);
    out.textureIndex = textureIndices ? textureIndices[index] : 0;

    // --- 不透明度 ---
    const float* opacities = csmGetDrawableOpacities(model->model_);
    out.opacity = opacities ? opacities[index] : 1.0f;

    // --- 渲染顺序 ---
    // Cubism Core 4.x 与 5.x/6.x 的 API 名字不同：
    //   旧版 csmGetDrawableRenderOrders(model) / 新版 csmGetRenderOrders(model)
    // CMake 会读 SDK 头文件判断实际提供的是哪个，并定义对应宏。
    // 两者语义一致：返回全部部件的渲染顺序数组，按 index 取当前部件的值。
#if defined(PPET_CUBISM_HAS_RENDER_ORDERS)
    const int* renderOrders = csmGetRenderOrders(model->model_);
#elif defined(PPET_CUBISM_HAS_DRAWABLE_RENDER_ORDERS)
    const int* renderOrders = csmGetDrawableRenderOrders(model->model_);
#else
    // 未探测到（理论上不会发生：CMake 找不到任一 API 时会直接报错）。
    // 退回到 DrawOrder，至少能保证绘制次序不乱。
    const int* renderOrders = csmGetDrawableDrawOrders(model->model_);
#endif
    out.renderOrder = renderOrders ? renderOrders[index] : index;

    // --- 混合模式与双面标志 ---
    const csmFlags* constantFlags = csmGetDrawableConstantFlags(model->model_);
    out.additiveBlend = false;
    out.multiplicativeBlend = false;
    out.doubleSided = true;
    if (constantFlags) {
        const csmFlags flags = constantFlags[index];
        out.additiveBlend = (flags & csmBlendAdditive) != 0;
        out.multiplicativeBlend = (flags & csmBlendMultiplicative) != 0;
        out.doubleSided = (flags & csmIsDoubleSided) != 0;
    }

    // --- 可见性 ---
    const csmFlags* dynamicFlags = csmGetDrawableDynamicFlags(model->model_);
    out.visible = true;
    if (dynamicFlags) {
        out.visible = (dynamicFlags[index] & csmIsVisible) != 0;
    }

    return true;
#else
    (void)model;
    (void)index;
    (void)out;
    return false;
#endif
}

}  // namespace ppet::live2d::cubism
