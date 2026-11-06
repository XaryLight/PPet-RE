// PPet - C++ 移植版
// Cubism Core 桩实现（Stub）——公开头文件。
//
// 【为什么需要桩】
//   Live2D Cubism SDK for Native 是专有许可软件，不能随本项目分发。
//   因此本项目把「Cubism 调用」全部收敛到 CubismCoreAdapter.cpp 一个文件里，
//   并用下面的桩实现替换真实 SDK，使得适配层与上层逻辑（JSON 解析、
//   纹理路径解析、视线追踪参数映射）都能在没有 SDK 的环境下被编译与运行验证。
//
// 【与真实 SDK 的关系】
//   本文件的函数名、参数、返回值与 Cubism Core 4.x/5.x 的
//   Live2DCubismCore.h **完全一致**（依据官方《Live2D Cubism Core API
//   Reference》r12/r13 文档），因此 CubismCoreAdapter.cpp 的同一份源码
//   既能链接真实 SDK，也能链接本桩，无需任何条件编译。
//
//   与真实 SDK 的差异仅限于「数据内容」：
//     - 真实 SDK 解析二进制 .moc3；本桩同样使用**二进制**的桩 moc 文件，
//       但格式是本项目自定义的简单格式（见下），便于测试精确控制
//       参数表与部件表。
//     - 真实 SDK 的 csmGetDrawableVertexPositions() 返回 Core 内部缓冲区；
//       本桩返回模型内存块内部的等效数据。
//
// 【桩 moc 文件格式】（全部为小端）
//   头部（40 字节）：
//     char     magic[8]   = "PPETMOC1"
//     uint32   version            = 1
//     uint32   paramCount
//     uint32   drawableCount
//     float    canvasWidth
//     float    canvasHeight
//     float    canvasOriginX
//     float    canvasOriginY
//     float    pixelsPerUnit
//   参数记录 × paramCount：
//     uint16   idLen
//     char     id[idLen]
//     float    defaultValue
//     float    minValue
//     float    maxValue
//   部件记录 × drawableCount：
//     uint16   idLen
//     char     id[idLen]
//     int32    textureIndex
//     float    opacity
//     uint32   vertexCount
//     uint32   indexCount
//     float    vertices[vertexCount * 2]
//     float    uvs[vertexCount * 2]
//     uint16   indices[indexCount]
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 类型定义（与真实 Live2DCubismCore.h 一致）
// ---------------------------------------------------------------------------

/// Cubism 版本号：Major(1字节) + Minor(1字节) + Patch(2字节)。
typedef uint32_t csmVersion;

/// moc3 文件格式版本号。
typedef uint32_t csmMocVersion;

/// 标志位集合。
typedef uint8_t csmFlags;

/// 二维向量。真实 SDK 中同样是 { float X; float Y; } 的结构体。
typedef struct {
    float X;
    float Y;
} csmVector2;

/// 四维向量（用于 Multiply / Screen 颜色）。
typedef struct {
    float X;
    float Y;
    float Z;
    float W;
} csmVector4;

/// 模型（不透明句柄）。
typedef struct CSM_MODEL CSM_MODEL;
typedef CSM_MODEL csmModel;

/// moc 数据（不透明句柄）。
typedef struct CSM_MOC CSM_MOC;
typedef CSM_MOC csmMoc;

// ---------------------------------------------------------------------------
// 对齐约束（真实 SDK 写在 Live2DCubismCore.h 的 enum 中）
// ---------------------------------------------------------------------------

enum {
    /** moc 内存所需的对齐字节数。 */
    csmAlignofMoc = 64,
    /** model 内存所需的对齐字节数。 */
    csmAlignofModel = 16
};

// ---------------------------------------------------------------------------
// moc3 文件版本常量
// ---------------------------------------------------------------------------

enum {
    csmMocVersion_Unknown = 0,
    csmMocVersion_30 = 1,
    csmMocVersion_33 = 2,
    csmMocVersion_40 = 3,
    csmMocVersion_42 = 4,
    csmMocVersion_50 = 5
};

// ---------------------------------------------------------------------------
// 动态标志位（csmGetDrawableDynamicFlags）
// ---------------------------------------------------------------------------

enum {
    /** 部件可见。 */
    csmIsVisible = 1 << 0,
    /** 可见性发生变化。 */
    csmVisibilityDidChange = 1 << 1,
    /** 不透明度发生变化。 */
    csmOpacityDidChange = 1 << 2,
    /** DrawOrder 发生变化。 */
    csmDrawOrderDidChange = 1 << 3,
    /** RenderOrder 发生变化。 */
    csmRenderOrderDidChange = 1 << 4,
    /** 顶点坐标发生变化。 */
    csmVertexPositionsDidChange = 1 << 5
};

// ---------------------------------------------------------------------------
// 常量标志位（csmGetDrawableConstantFlags）
// ---------------------------------------------------------------------------

enum {
    /** 加法混合。 */
    csmBlendAdditive = 1 << 0,
    /** 乘法混合。 */
    csmBlendMultiplicative = 1 << 1,
    /** 双面渲染。 */
    csmIsDoubleSided = 1 << 2,
    /** 反转遮罩。 */
    csmIsInvertedMask = 1 << 3
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// 返回 Core 版本号。
csmVersion csmGetVersion(void);

/// 返回 Core 能处理的最新 moc3 文件版本。
csmMocVersion csmGetLatestMocVersion(void);

/// 返回给定内存中的 moc3 文件版本；不是 moc3 时返回 csmMocVersion_Unknown。
csmMocVersion csmGetMocVersion(const void* address, const unsigned int size);

/// 在指定内存上「复活」moc 结构。
/// @param address 内存首地址（须按 csmAlignofMoc 对齐）
/// @param size    内存字节数
/// @return 成功返回 moc 指针，失败返回 NULL。
csmMoc* csmReviveMocInPlace(void* address, const unsigned int size);

/// 返回由 moc 生成的 model 结构所需字节数。
unsigned int csmGetSizeofModel(const csmMoc* moc);

/// 在指定内存上初始化 model 结构。
/// @param moc     moc 指针
/// @param address 内存首地址（须按 csmAlignofModel 对齐）
/// @param size    内存字节数
/// @return 成功返回 model 指针，失败返回 NULL。
csmModel* csmInitializeModelInPlace(const csmMoc* moc, void* address,
                                    const unsigned int size);

/// 依据当前参数重算顶点等动态数据。
void csmUpdateModel(csmModel* model);

/// 读取画布信息。
/// @param model            模型
/// @param outSizeInPixels  画布尺寸（像素）
/// @param outOriginInPixels 画布中心（像素）
/// @param outPixelsPerUnit 单位像素数
void csmReadCanvasInfo(const csmModel* model, csmVector2* outSizeInPixels,
                        csmVector2* outOriginInPixels, float* outPixelsPerUnit);

/// 参数数量。
int csmGetParameterCount(const csmModel* model);

/// 参数名数组（长度与参数数量一致）。
const char** csmGetParameterIds(const csmModel* model);

/// 参数当前值数组（可写）。
float* csmGetParameterValues(csmModel* model);

/// 参数默认值数组。
const float* csmGetParameterDefaultValues(const csmModel* model);

/// 参数下界数组。
const float* csmGetParameterMinimumValues(const csmModel* model);

/// 参数上界数组。
const float* csmGetParameterMaximumValues(const csmModel* model);

/// 部件数量。
int csmGetDrawableCount(const csmModel* model);

/// 部件名数组。
const char** csmGetDrawableIds(const csmModel* model);

/// 部件常量标志数组。
const csmFlags* csmGetDrawableConstantFlags(const csmModel* model);

/// 部件动态标志数组。
const csmFlags* csmGetDrawableDynamicFlags(const csmModel* model);

/// 部件纹理序号数组。
const int* csmGetDrawableTextureIndices(const csmModel* model);

/// 部件 DrawOrder 数组。
const int* csmGetDrawableDrawOrders(const csmModel* model);

/// 部件 RenderOrder 数组。
///
/// 对应真实 SDK 6.x 的 csmGetRenderOrders。
/// 历史版本曾用 csmGetDrawableRenderOrders，这里保留同名别名以便旧代码继续编译。
const int* csmGetRenderOrders(const csmModel* model);

/// csmGetRenderOrders 的旧别名。
const int* csmGetDrawableRenderOrders(const csmModel* model);

/// 部件不透明度数组。
const float* csmGetDrawableOpacities(const csmModel* model);

/// 部件顶点数数组。
const int* csmGetDrawableVertexCounts(const csmModel* model);

/// 部件顶点坐标：**锯齿数组**，第 d 项指向第 d 个部件自己的顶点数组。
const csmVector2** csmGetDrawableVertexPositions(const csmModel* model);

/// 部件纹理坐标：**锯齿数组**，语义同 csmGetDrawableVertexPositions。
const csmVector2** csmGetDrawableVertexUvs(const csmModel* model);

/// 部件索引数量数组。
const int* csmGetDrawableIndexCounts(const csmModel* model);

/// 部件三角形索引：**锯齿数组**，索引空间按部件独立。
const unsigned short** csmGetDrawableIndices(const csmModel* model);

/// 复位部件动态标志（绘制结束后调用）。
void csmResetDrawableDynamicFlags(csmModel* model);

#ifdef __cplusplus
}
#endif
