// PPet - C++ 移植版
// Cubism Core 桩实现（Stub）——实现文件。
//
// 本文件实现 CubismStubCore.h 中声明的全部函数，行为与真实 Cubism Core
// 4.x/5.x 在「接口层面」一致：
//   - 同样的 InPlace 内存模型（csmReviveMocInPlace / csmGetSizeofModel /
//     csmInitializeModelInPlace），调用序列完全相同；
//   - 同样的「模型状态全部位于调用方提供的内存块内」的特性，
//     因此适配层「释放模型 = 释放内存块」的约定对桩与真 SDK 都成立；
//   - 同样的锯齿数组语义（每个 drawable 拥有独立的顶点/UV/索引数组，
//     索引空间按 drawable 独立）；
//   - 同样的绘制顺序语义（csmGetRenderOrders 给出实际渲染顺序）。
//
// 差异仅在数据内容：真实 SDK 解析二进制 .moc3，本桩解析本项目自定义的
// 二进制桩格式（格式说明见 CubismStubCore.h 顶部注释）。
#include <tools/test_stubs/CubismStub/CubismStubCore.h>

#include <cmath>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// 桩 moc 文件的二进制读取工具
// ---------------------------------------------------------------------------

/// 顺序读取器：按小端逐字段读取，越界即失败。
class ByteReader {
public:
    ByteReader(const unsigned char* data, std::size_t size)
        : data_(data), size_(size) {}

    /// 剩余字节数。
    std::size_t Remaining() const { return size_ - offset_; }

    /// 读取 uint16。
    bool ReadU16(uint16_t& out) {
        if (Remaining() < 2) return false;
        out = static_cast<uint16_t>(data_[offset_]) |
              static_cast<uint16_t>(static_cast<uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return true;
    }

    /// 读取 uint32。
    bool ReadU32(uint32_t& out) {
        if (Remaining() < 4) return false;
        out = static_cast<uint32_t>(data_[offset_]) |
              (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
              (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
              (static_cast<uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return true;
    }

    /// 读取 int32。
    bool ReadI32(int32_t& out) {
        uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        out = static_cast<int32_t>(raw);
        return true;
    }

    /// 读取 float（按位复制，避免类型双关问题）。
    bool ReadF32(float& out) {
        uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        std::memcpy(&out, &raw, sizeof(out));
        return true;
    }

    /// 跳过 count 个字节。
    bool Skip(std::size_t count) {
        if (Remaining() < count) return false;
        offset_ += count;
        return true;
    }

    /// 读取固定长度的字节（用于 id）。
    bool ReadBytes(unsigned char* out, std::size_t count) {
        if (Remaining() < count) return false;
        std::memcpy(out, data_ + offset_, count);
        offset_ += count;
        return true;
    }

private:
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

/// 桩 moc 文件头部的常量。
constexpr char kStubMagic[8] = {'P', 'P', 'E', 'T', 'M', 'O', 'C', '1'};
constexpr uint32_t kStubFormatVersion = 1;

/// 头部布局：
///   char   magic[8]
///   uint32 version
///   uint32 payloadSize   —— 整个文件（含头部）的字节数
///   uint32 paramCount
///   uint32 drawableCount
///   float  canvasWidth
///   float  canvasHeight
///   float  canvasOriginX
///   float  canvasOriginY
///   float  pixelsPerUnit
constexpr std::size_t kStubHeaderSize = 8 + 4 + 4 + 4 + 4 + 20;

/// moc 文件头部的解析结果。
struct StubHeader {
    uint32_t payloadSize = 0;
    uint32_t paramCount = 0;
    uint32_t drawableCount = 0;
    float canvasWidth = 0.0f;
    float canvasHeight = 0.0f;
    float canvasOriginX = 0.0f;
    float canvasOriginY = 0.0f;
    float pixelsPerUnit = 1.0f;
};

/// 解析文件头。失败返回 false。
///
/// @param data   文件首地址
/// @param size   已知字节数（可以小于 payloadSize，此时按 size 截断）
bool ParseHeader(const unsigned char* data, std::size_t size, StubHeader& out) {
    if (size < kStubHeaderSize) return false;
    if (std::memcmp(data, kStubMagic, sizeof(kStubMagic)) != 0) return false;

    ByteReader reader(data, size);
    if (!reader.Skip(sizeof(kStubMagic))) return false;

    uint32_t version = 0;
    if (!reader.ReadU32(version)) return false;
    if (version != kStubFormatVersion) return false;
    if (!reader.ReadU32(out.payloadSize)) return false;
    if (out.payloadSize < kStubHeaderSize) return false;
    if (!reader.ReadU32(out.paramCount)) return false;
    if (!reader.ReadU32(out.drawableCount)) return false;
    if (!reader.ReadF32(out.canvasWidth)) return false;
    if (!reader.ReadF32(out.canvasHeight)) return false;
    if (!reader.ReadF32(out.canvasOriginX)) return false;
    if (!reader.ReadF32(out.canvasOriginY)) return false;
    if (!reader.ReadF32(out.pixelsPerUnit)) return false;
    return true;
}

/// 扫描整个桩 moc 文件，得出各数组规模与 id 总长度。
bool ScanExtents(const unsigned char* data, std::size_t size, StubHeader& header,
                 std::size_t& totalParamIdBytes, std::size_t& totalDrawableIdBytes,
                 std::size_t& totalVertices, std::size_t& totalIndices) {
    if (!ParseHeader(data, size, header)) return false;

    // 以头部声明的总长度为准，但不超过实际可读长度。
    const std::size_t usable = (header.payloadSize <= size) ? header.payloadSize : size;

    ByteReader reader(data, usable);
    if (!reader.Skip(kStubHeaderSize)) return false;

    totalParamIdBytes = 0;
    for (uint32_t i = 0; i < header.paramCount; ++i) {
        uint16_t idLen = 0;
        if (!reader.ReadU16(idLen)) return false;
        if (!reader.Skip(idLen)) return false;
        if (!reader.Skip(12)) return false;  // default/min/max
        totalParamIdBytes += static_cast<std::size_t>(idLen) + 1;
    }

    totalDrawableIdBytes = 0;
    totalVertices = 0;
    totalIndices = 0;
    for (uint32_t i = 0; i < header.drawableCount; ++i) {
        uint16_t idLen = 0;
        if (!reader.ReadU16(idLen)) return false;
        if (!reader.Skip(idLen)) return false;
        if (!reader.Skip(4)) return false;   // textureIndex
        if (!reader.Skip(4)) return false;   // opacity
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        if (!reader.ReadU32(vertexCount)) return false;
        if (!reader.ReadU32(indexCount)) return false;
        if (!reader.Skip(static_cast<std::size_t>(vertexCount) * 8)) return false;
        if (!reader.Skip(static_cast<std::size_t>(vertexCount) * 8)) return false;
        if (!reader.Skip(static_cast<std::size_t>(indexCount) * 2)) return false;
        totalDrawableIdBytes += static_cast<std::size_t>(idLen) + 1;
        totalVertices += vertexCount;
        totalIndices += indexCount;
    }
    return true;
}

/// 内存块内的对齐分配器。
///
/// 真实 SDK 的模型内存是一整块连续内存，内部所有指针都指向该块。
/// 桩实现用同一个分配器复刻这一点，从而让适配层的内存管理代码
/// 对桩与真 SDK 完全一致。
class BlockAllocator {
public:
    BlockAllocator(unsigned char* base, std::size_t capacity)
        : base_(base), capacity_(capacity) {}

    /// 分配 count 个 T，并按至少 16 字节对齐。
    template <typename T>
    T* Alloc(std::size_t count) {
        const std::size_t alignment = alignof(T) > 16 ? alignof(T) : 16;
        const std::size_t bytes = count * sizeof(T);
        const std::size_t offset = AlignUp(offset_, alignment);
        if (offset + bytes > capacity_) return nullptr;
        T* result = reinterpret_cast<T*>(base_ + offset);
        offset_ = offset + bytes;
        return result;
    }

    /// 分配 count 个字节（原始存储）。
    void* AllocRaw(std::size_t count) {
        const std::size_t offset = AlignUp(offset_, 16);
        if (offset + count > capacity_) return nullptr;
        void* result = base_ + offset;
        offset_ = offset + count;
        return result;
    }

private:
    static std::size_t AlignUp(std::size_t value, std::size_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    unsigned char* base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t offset_ = 0;
};

/// 对齐到 16 字节。
std::size_t AlignUp16(std::size_t value) { return (value + 15) / 16 * 16; }

}  // namespace

// ---------------------------------------------------------------------------
// 真实 SDK 的 csmMoc / csmModel 是不透明结构，这里给出等价实现。
// ---------------------------------------------------------------------------

/// 桩 moc 句柄。
///
/// 真实 SDK 的 csmMoc 是一大块解析后的二进制结构，直接覆盖在 moc 数据之上。
/// 桩实现不写入任何内容（避免破坏原始 moc 数据），只把句柄当作
/// 「指向原始 moc 数据的指针」使用。
struct CSM_MOC {
    /// 桩 moc 的魔数（仅用于标识句柄有效）。
    unsigned int magic;
};

/// 桩 moc 的魔数。
static const unsigned int kStubMocMagic = 0x50435453u;  // "PCTS"

/// 模型内存块的布局头部。
///
/// 所有成员指针都指向同一块内存（调用方提供的 modelMemory）内部，
/// 与真实 SDK 的 csmModel 结构等价。
struct CSM_MODEL {
    // --- 画布信息 ---
    float canvasWidth;
    float canvasHeight;
    float canvasOriginX;
    float canvasOriginY;
    float pixelsPerUnit;

    // --- 参数 ---
    unsigned int paramCount;
    const char** paramIds;
    float* paramValues;
    float* paramDefaults;
    float* paramMinimums;
    float* paramMaximums;

    // --- 部件 ---
    unsigned int drawableCount;
    const char** drawableIds;
    csmFlags* constantFlags;
    csmFlags* dynamicFlags;
    int* textureIndices;
    int* drawOrders;
    int* renderOrders;
    float* opacities;
    /// 每个部件的遮罩数量（桩恒为 0）。
    int* maskCounts;
    /// 每个部件的遮罩编号指针（桩恒为 nullptr）。
    const int** maskLists;
    int* vertexCounts;
    int* indexCounts;

    // 锯齿数组：第 d 项指向第 d 个部件自己的数据（都在同一块内存内）。
    const csmVector2** vertexPositions;
    const csmVector2** vertexUvs;
    const unsigned short** indices;

    // 基准顶点（未被参数变换），每次 Update 从这里重算，避免旋转累积。
    csmVector2* baseVertexPositions;
    /// 每个部件在 baseVertexPositions 中的起始下标与数量。
    unsigned int* baseVertexOffsets;
    unsigned int* baseVertexCounts;
};

extern "C" {

csmVersion csmGetVersion(void) {
    // 4.3.0（真实 SDK 的版本号按 Major/Minor/Patch 打包成 4 字节）
    return (4u << 24) | (3u << 16) | 0u;
}

csmMocVersion csmGetLatestMocVersion(void) {
    return csmMocVersion_50;
}

csmMocVersion csmGetMocVersion(const void* address, const unsigned int size) {
    if (!address || size < kStubHeaderSize) return csmMocVersion_Unknown;
    StubHeader header;
    if (!ParseHeader(static_cast<const unsigned char*>(address), size, header)) {
        return csmMocVersion_Unknown;
    }
    return csmMocVersion_50;
}

csmMoc* csmReviveMocInPlace(void* address, const unsigned int size) {
    if (!address || size < kStubHeaderSize) return nullptr;

    // 真实 SDK 会在这里检查 moc 版本与内存对齐，并把 csmMoc 结构覆盖到
    // 传入的内存上。桩实现不写入内存（原始 moc 数据必须保持完整），
    // 只做合法性检查后返回句柄。
    StubHeader header;
    if (!ParseHeader(static_cast<const unsigned char*>(address), size, header)) {
        return nullptr;
    }
    if (header.payloadSize > size) return nullptr;

    return static_cast<csmMoc*>(address);
}

unsigned int csmGetSizeofModel(const csmMoc* moc) {
    if (!moc) return 0;

    const auto* data = reinterpret_cast<const unsigned char*>(moc);
    StubHeader header;
    std::size_t paramIdBytes = 0;
    std::size_t drawableIdBytes = 0;
    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    if (!ScanExtents(data, header.payloadSize, header, paramIdBytes, drawableIdBytes,
                     totalVertices, totalIndices)) {
        return 0;
    }

    const std::size_t paramCount = header.paramCount;
    const std::size_t drawableCount = header.drawableCount;

    std::size_t total = AlignUp16(sizeof(CSM_MODEL));
    // 参数 id 字符串池 + id 指针数组 + 4 组 float 数组。
    total = AlignUp16(total + paramIdBytes);
    total = AlignUp16(total + paramCount * sizeof(const char*));
    total = AlignUp16(total + paramCount * sizeof(float) * 4);
    // 部件 id 字符串池 + id 指针数组 + 标志与数值数组。
    total = AlignUp16(total + drawableIdBytes);
    total = AlignUp16(total + drawableCount * sizeof(const char*));
    total = AlignUp16(total + drawableCount * sizeof(csmFlags) * 2);
    total = AlignUp16(total + drawableCount * sizeof(int) * 5);
    total = AlignUp16(total + drawableCount * sizeof(float));
    // 锯齿数组的指针表。
    total = AlignUp16(total + drawableCount * sizeof(const csmVector2*) * 2);
    total = AlignUp16(total + drawableCount * sizeof(const unsigned short*));
    total = AlignUp16(total + drawableCount * sizeof(unsigned int) * 2);
    // 顶点 / UV / 基准顶点 / 索引。
    total = AlignUp16(total + totalVertices * sizeof(csmVector2) * 3);
    total = AlignUp16(total + totalIndices * sizeof(unsigned short));

    return static_cast<unsigned int>(total);
}

csmModel* csmInitializeModelInPlace(const csmMoc* moc, void* address,
                                     const unsigned int size) {
    if (!moc || !address) return nullptr;

    const auto* raw = reinterpret_cast<const unsigned char*>(moc);

    StubHeader header;
    std::size_t paramIdBytes = 0;
    std::size_t drawableIdBytes = 0;
    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    if (!ScanExtents(raw, header.payloadSize, header, paramIdBytes, drawableIdBytes,
                     totalVertices, totalIndices)) {
        return nullptr;
    }
    const std::size_t mocSize = header.payloadSize;

    auto* model = static_cast<CSM_MODEL*>(address);
    auto* base = static_cast<unsigned char*>(address);
    BlockAllocator alloc(base, size);

    model->canvasWidth = header.canvasWidth;
    model->canvasHeight = header.canvasHeight;
    model->canvasOriginX = header.canvasOriginX;
    model->canvasOriginY = header.canvasOriginY;
    model->pixelsPerUnit = header.pixelsPerUnit;

    const std::size_t paramCount = header.paramCount;
    const std::size_t drawableCount = header.drawableCount;

    // --- 分配固定大小的数组 ---
    model->paramIds = alloc.Alloc<const char*>(paramCount);
    model->paramValues = alloc.Alloc<float>(paramCount);
    model->paramDefaults = alloc.Alloc<float>(paramCount);
    model->paramMinimums = alloc.Alloc<float>(paramCount);
    model->paramMaximums = alloc.Alloc<float>(paramCount);
    model->paramCount = static_cast<unsigned int>(paramCount);

    model->drawableIds = alloc.Alloc<const char*>(drawableCount);
    model->constantFlags = alloc.Alloc<csmFlags>(drawableCount);
    model->dynamicFlags = alloc.Alloc<csmFlags>(drawableCount);
    model->textureIndices = alloc.Alloc<int>(drawableCount);
    model->drawOrders = alloc.Alloc<int>(drawableCount);
    model->renderOrders = alloc.Alloc<int>(drawableCount);
    model->opacities = alloc.Alloc<float>(drawableCount);
    model->maskCounts = alloc.Alloc<int>(drawableCount);
    model->maskLists = alloc.Alloc<const int*>(drawableCount);
    model->vertexCounts = alloc.Alloc<int>(drawableCount);
    model->indexCounts = alloc.Alloc<int>(drawableCount);
    model->drawableCount = static_cast<unsigned int>(drawableCount);

    model->vertexPositions = alloc.Alloc<const csmVector2*>(drawableCount);
    model->vertexUvs = alloc.Alloc<const csmVector2*>(drawableCount);
    model->indices = alloc.Alloc<const unsigned short*>(drawableCount);
    model->baseVertexOffsets = alloc.Alloc<unsigned int>(drawableCount);
    model->baseVertexCounts = alloc.Alloc<unsigned int>(drawableCount);

    // --- 分配 id 字符串池 ---
    char* paramIdPool = static_cast<char*>(alloc.AllocRaw(paramIdBytes));
    char* drawableIdPool = static_cast<char*>(alloc.AllocRaw(drawableIdBytes));
    if (!model->paramIds || !model->vertexPositions || !model->baseVertexOffsets ||
        (paramIdBytes > 0 && !paramIdPool) ||
        (drawableIdBytes > 0 && !drawableIdPool)) {
        return nullptr;
    }

    // --- 顶点 / UV / 索引池 ---
    csmVector2* vertexPool = alloc.Alloc<csmVector2>(totalVertices);
    csmVector2* uvPool = alloc.Alloc<csmVector2>(totalVertices);
    csmVector2* basePool = alloc.Alloc<csmVector2>(totalVertices);
    unsigned short* indexPool = alloc.Alloc<unsigned short>(totalIndices);
    if ((totalVertices > 0 && (!vertexPool || !uvPool || !basePool)) ||
        (totalIndices > 0 && !indexPool)) {
        return nullptr;
    }

    // --- 逐条读取参数 ---
    ByteReader reader(raw, mocSize);
    if (!reader.Skip(kStubHeaderSize)) return nullptr;

    char* paramIdCursor = paramIdPool;
    for (std::size_t i = 0; i < paramCount; ++i) {
        uint16_t idLen = 0;
        if (!reader.ReadU16(idLen)) return nullptr;
        if (!reader.ReadBytes(reinterpret_cast<unsigned char*>(paramIdCursor), idLen)) {
            return nullptr;
        }
        paramIdCursor[idLen] = '\0';
        model->paramIds[i] = paramIdCursor;
        paramIdCursor += idLen + 1;

        float defaultValue = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        if (!reader.ReadF32(defaultValue)) return nullptr;
        if (!reader.ReadF32(minValue)) return nullptr;
        if (!reader.ReadF32(maxValue)) return nullptr;
        model->paramValues[i] = defaultValue;
        model->paramDefaults[i] = defaultValue;
        model->paramMinimums[i] = minValue;
        model->paramMaximums[i] = maxValue;
    }

    // --- 逐条读取部件 ---
    char* drawableIdCursor = drawableIdPool;
    std::size_t vertexCursor = 0;
    std::size_t indexCursor = 0;
    for (std::size_t d = 0; d < drawableCount; ++d) {
        uint16_t idLen = 0;
        if (!reader.ReadU16(idLen)) return nullptr;
        if (!reader.ReadBytes(reinterpret_cast<unsigned char*>(drawableIdCursor), idLen)) {
            return nullptr;
        }
        drawableIdCursor[idLen] = '\0';
        model->drawableIds[d] = drawableIdCursor;
        drawableIdCursor += idLen + 1;

        int32_t textureIndex = 0;
        float opacity = 1.0f;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        if (!reader.ReadI32(textureIndex)) return nullptr;
        if (!reader.ReadF32(opacity)) return nullptr;
        if (!reader.ReadU32(vertexCount)) return nullptr;
        if (!reader.ReadU32(indexCount)) return nullptr;

        model->textureIndices[d] = textureIndex;
        model->opacities[d] = opacity;
        model->vertexCounts[d] = static_cast<int>(vertexCount);
        model->indexCounts[d] = static_cast<int>(indexCount);
        // Live2D 网格多为双面，桩实现统一标记为双面。
        model->constantFlags[d] = csmIsDoubleSided;
        // 桩不建立遮罩关系：数量为 0，编号指针为空。
        model->maskCounts[d] = 0;
        model->maskLists[d] = nullptr;
        model->dynamicFlags[d] = csmIsVisible;
        model->drawOrders[d] = static_cast<int>(d);
        model->renderOrders[d] = static_cast<int>(d);

        model->baseVertexOffsets[d] = static_cast<unsigned int>(vertexCursor);
        model->baseVertexCounts[d] = vertexCount;

        csmVector2* verts = vertexPool + vertexCursor;
        csmVector2* uvs = uvPool + vertexCursor;
        csmVector2* baseVerts = basePool + vertexCursor;

        for (uint32_t v = 0; v < vertexCount; ++v) {
            float x = 0.0f;
            float y = 0.0f;
            if (!reader.ReadF32(x)) return nullptr;
            if (!reader.ReadF32(y)) return nullptr;
            verts[v] = {x, y};
            baseVerts[v] = {x, y};
        }
        for (uint32_t v = 0; v < vertexCount; ++v) {
            float u = 0.0f;
            float w = 0.0f;
            if (!reader.ReadF32(u)) return nullptr;
            if (!reader.ReadF32(w)) return nullptr;
            uvs[v] = {u, w};
        }

        unsigned short* idx = indexPool + indexCursor;
        for (uint32_t i = 0; i < indexCount; ++i) {
            uint16_t value = 0;
            if (!reader.ReadU16(value)) return nullptr;
            idx[i] = value;
        }

        model->vertexPositions[d] = verts;
        model->vertexUvs[d] = uvs;
        model->indices[d] = idx;

        vertexCursor += vertexCount;
        indexCursor += indexCount;
    }

    return model;
}

void csmUpdateModel(csmModel* model) {
    if (!model) return;

    // 真实 SDK 会依据参数重算顶点并更新动态标志。
    // 桩实现把「参数值」映射到部件顶点上，用于验证参数写入路径确实生效：
    // 部件名含 "Angle" 的部件随 ParamAngleX/Y 旋转，其余部件保持静止。
    float angleX = 0.0f;
    float angleY = 0.0f;
    for (unsigned int i = 0; i < model->paramCount; ++i) {
        const char* id = model->paramIds[i];
        if (!id) continue;
        if (std::strcmp(id, "ParamAngleX") == 0) angleX = model->paramValues[i];
        if (std::strcmp(id, "ParamAngleY") == 0) angleY = model->paramValues[i];
    }

    for (unsigned int d = 0; d < model->drawableCount; ++d) {
        const char* id = model->drawableIds[d];
        const bool isAnglePart = id && std::strstr(id, "Angle") != nullptr;
        csmVector2* verts = const_cast<csmVector2*>(model->vertexPositions[d]);
        const csmVector2* base = model->baseVertexPositions + model->baseVertexOffsets[d];
        const unsigned int count = model->baseVertexCounts[d];

        if (isAnglePart) {
            const float rad = angleX * 3.14159265f / 180.0f;
            const float cosA = std::cos(rad);
            const float sinA = std::sin(rad);
            // 始终从基准顶点重算，否则多次 Update 会让旋转不断累积。
            for (unsigned int v = 0; v < count; ++v) {
                const float x = base[v].X;
                const float y = base[v].Y;
                verts[v].X = x * cosA - y * sinA;
                verts[v].Y = x * sinA + y * cosA + angleY * 0.001f;
            }
        } else {
            for (unsigned int v = 0; v < count; ++v) verts[v] = base[v];
        }

        // 动态标志：可见性 + 顶点变化。
        model->dynamicFlags[d] = csmIsVisible | csmVertexPositionsDidChange;
        if (model->opacities[d] <= 0.0f) {
            model->dynamicFlags[d] =
                static_cast<csmFlags>(model->dynamicFlags[d] & ~csmIsVisible);
        }
    }
}

void csmReadCanvasInfo(const csmModel* model, csmVector2* outSizeInPixels,
                        csmVector2* outOriginInPixels, float* outPixelsPerUnit) {
    if (!model) return;
    if (outSizeInPixels) {
        outSizeInPixels->X = model->canvasWidth;
        outSizeInPixels->Y = model->canvasHeight;
    }
    if (outOriginInPixels) {
        outOriginInPixels->X = model->canvasOriginX;
        outOriginInPixels->Y = model->canvasOriginY;
    }
    if (outPixelsPerUnit) {
        *outPixelsPerUnit = model->pixelsPerUnit;
    }
}

int csmGetParameterCount(const csmModel* model) {
    if (!model) return 0;
    return static_cast<int>(model->paramCount);
}

const char** csmGetParameterIds(const csmModel* model) {
    if (!model || model->paramCount == 0) return nullptr;
    return model->paramIds;
}

float* csmGetParameterValues(csmModel* model) {
    if (!model || model->paramCount == 0) return nullptr;
    return model->paramValues;
}

const float* csmGetParameterDefaultValues(const csmModel* model) {
    if (!model || model->paramCount == 0) return nullptr;
    return model->paramDefaults;
}

const float* csmGetParameterMinimumValues(const csmModel* model) {
    if (!model || model->paramCount == 0) return nullptr;
    return model->paramMinimums;
}

const float* csmGetParameterMaximumValues(const csmModel* model) {
    if (!model || model->paramCount == 0) return nullptr;
    return model->paramMaximums;
}

int csmGetDrawableCount(const csmModel* model) {
    if (!model) return 0;
    return static_cast<int>(model->drawableCount);
}

const char** csmGetDrawableIds(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->drawableIds;
}

const csmFlags* csmGetDrawableConstantFlags(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->constantFlags;
}

const csmFlags* csmGetDrawableDynamicFlags(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->dynamicFlags;
}

const int* csmGetDrawableTextureIndices(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->textureIndices;
}

const int* csmGetDrawableDrawOrders(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->drawOrders;
}

const int* csmGetRenderOrders(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->renderOrders;
}

const int* csmGetDrawableRenderOrders(const csmModel* model) {
    return csmGetRenderOrders(model);
}

const float* csmGetDrawableOpacities(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->opacities;
}

const int* csmGetDrawableMaskCounts(const csmModel* model) {
    // 桩不生成遮罩关系：每个部件的遮罩数量恒为 0。
    if (!model || model->drawableCount == 0) return nullptr;
    return model->maskCounts;
}

const int** csmGetDrawableMasks(const csmModel* model) {
    // 桩没有遮罩：返回空的锯齿数组（每项都是 nullptr）。
    if (!model || model->drawableCount == 0) return nullptr;
    return model->maskLists;
}

const int* csmGetDrawableVertexCounts(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->vertexCounts;
}

const csmVector2** csmGetDrawableVertexPositions(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->vertexPositions;
}

const csmVector2** csmGetDrawableVertexUvs(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->vertexUvs;
}

const int* csmGetDrawableIndexCounts(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->indexCounts;
}

const unsigned short** csmGetDrawableIndices(const csmModel* model) {
    if (!model || model->drawableCount == 0) return nullptr;
    return model->indices;
}

void csmResetDrawableDynamicFlags(csmModel* model) {
    if (!model) return;
    // 真实 SDK 会把「与上一次相比发生了变化」的位写进 dynamicFlags。
    // 桩实现直接清零，等价于「每帧全量刷新」。
    for (unsigned int d = 0; d < model->drawableCount; ++d) {
        model->dynamicFlags[d] = 0;
    }
}

}  // extern "C"
